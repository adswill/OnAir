// Pagers receiver: POCSAG (512, 1200, 2400 baud) and FLEX (1600/2, 3200/2, 3200/4, 6400/4) on one 25 kHz channel.
// Front end: mix out the signal offset, decimate to 96 kHz, filter to about +-10 kHz and decimate to 48 kHz, FM discriminator. The discriminator is
// kept as an unwrapped phase in a ring, so the mean frequency over any window (a symbol) is one subtraction. Four searchers look at every sample for
// a sync pattern: the POCSAG sync word at the three baud rates (the symbol sampling instants follow from the baud rate) and the FLEX sync 1 marker.
// The first that finds its pattern locks on (in either polarity) and reads the transmission; the symbol timing follows the data. The protocol layers
// are in pager_proto.cpp.
#include "dect2/pager_rx.h"
#include "dect2/dmr_dsp.h"
#include "dect2/exact_resampler.h"
#include "dect2/pager_proto.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMidRate = 96000.0;
constexpr double kWork = 48000.0;
constexpr size_t kRing = 8192, kMask = kRing - 1;
constexpr size_t kMaxMessages = 2000;
constexpr double kMinAmp = 0.2;                // rad/sample: the smallest mean deviation a sync may have (the full one is about 0.6)
constexpr double kTimingGain = 0.02;

int pickDecimation(double fs) {
    const int hi = std::max(1, (int)std::floor(fs / kMidRate));
    const int lo = std::max(1, (int)std::floor(fs / 130000.0));
    auto largestPrime = [](int v) {
        int lp = 1;
        for (int p = 2; p <= v; p++) while (v % p == 0) { lp = p; v /= p; }
        return lp;
    };
    int best = hi, bestPrime = largestPrime(hi);
    for (int d = hi; d >= lo; d--) {
        const int lp = largestPrime(d);
        if (lp <= 7) { best = d; bestPrime = lp; break; }
        if (lp < bestPrime) { best = d; bestPrime = lp; }
    }
    return best;
}

std::vector<int> splitFactors(int d) {
    std::vector<int> primes;
    for (int p = 2; d > 1;) {
        if (d % p == 0) { primes.push_back(p); d /= p; }
        else p++;
    }
    std::sort(primes.rbegin(), primes.rend());
    std::vector<int> stages;
    for (int p : primes) {
        if (!stages.empty() && stages.back() * p <= 8) stages.back() *= p;
        else stages.push_back(p);
    }
    return stages;
}

inline int popcount32(uint32_t v) { int n = 0; for (; v; v &= v - 1) n++; return n; }

struct Hit {
    bool ok = false;
    double score = 0;          // soft correlation with the pattern, rad/sample per symbol
    double mean = 0;           // the carrier offset: mean over the pattern, rad/sample
    int pol = 1;               // +1 as defined (1 = lower frequency), -1 inverted
    uint16_t code = 0;         // FLEX: the sync code
    int64_t at = 0;            // sample index of the end of the pattern
};

} // namespace

struct PagerReceiver::Impl {
    std::mutex mu;                         // guards the members up to pub
    std::function<void(const std::string&)> log;
    double rate = 0;                       // from configure()
    double offsetHz = 0;                   // setSignalOffset(): where the user's frequency sits in the input
    PagerTelemetry pub;                    // the published report
    std::atomic<bool> resetReq{true};

    // ---- receiver thread
    PagerTelemetry tel;
    double curRate = 0;
    bool built = false;
    int64_t nIn = 0;
    double nextReport = 0;
    double power = 0;                      // sum of |x|^2 of the input since the last report
    int64_t nPower = 0;
    std::vector<dmr::Fir<cf32>> stages;
    ExactResampler rs;
    bool rsPass = true;
    dmr::Fir<cf32> fin;
    double offPhase = 0;
    std::vector<cf32> bufA, bufB, mixBuf, rsOut, finOut, clean;

    // discriminator
    cf32 prev = cf32(0, 0);
    double phase = 0;
    std::vector<double> ring = std::vector<double>(kRing, 0.0);
    int64_t cur = -1;                      // index of the newest sample
    // channel power
    double blkPow = 0; int blkN = 0;
    double noiseFloor = 0, snrEma = 0;
    int sigHold = 0;
    bool haveFloor = false;

    // searchers
    struct Pat { double sps; int w; int off[32]; };
    Pat pat[3];                            // POCSAG
    Pat flexPat;                           // FLEX sync 1 (1600 baud)
    // peak search
    int peakH = -1; Hit best; int hold = 0;
    // lock
    bool locked = false;
    int lockH = -1;                        // 0 to 2 POCSAG, 3 FLEX
    int pol = 1;
    double m = 0, amp = 1;                 // symbol centre (carrier offset) and outer amplitude, rad/sample
    double tNext = 0, sps = 0; int w = 0, delta = 1;
    bool rebased = false;
    int speed = 0;
    // POCSAG
    pager::PocsagParser parser;
    int wi = 0, bi = 0; uint32_t curWord = 0; bool inSync = false;
    // FLEX
    int stage = 0, cnt = 0; uint32_t acc = 0;
    const pager::FlexMode* fmode = nullptr;
    double sx = 0, sa = 0; int sn = 0;
    std::vector<uint8_t> fb[4];
    int dataIdx = 0, dataTotal = 0;
    int64_t lockStart = 0;

    // results
    std::deque<PagerMessage> list;         // newest first
    bool listDirty = false;
    uint64_t serial = 0;
    int64_t lastLockEnd = -(int64_t)1e12;
    std::vector<PagerMessage> tmp;

    void say(const std::string& s) { std::function<void(const std::string&)> cb; { std::lock_guard<std::mutex> lk(mu); cb = log; } if (cb) cb(s); }
    double nowSec() const { return curRate > 0 ? (double)nIn / curRate : 0; }

    void design() {
        // searchers
        for (int i = 0; i < 3; i++) {
            const double baud = i == 0 ? 512 : (i == 1 ? 1200 : 2400);
            pat[i].sps = kWork / baud;
            pat[i].w = (int)std::lround(pat[i].sps);
            for (int k = 0; k < 32; k++) pat[i].off[k] = (int)std::lround((31 - k) * pat[i].sps);
        }
        flexPat.sps = kWork / 1600.0;
        flexPat.w = 30;
        for (int k = 0; k < 32; k++) flexPat.off[k] = (31 - k) * 30;
    }

    void resetState() {
        double r, off;
        { std::lock_guard<std::mutex> lk(mu); r = rate; off = offsetHz; curRate = r; }
        const uint64_t s = tel.seq;        // the report number goes on
        tel = PagerTelemetry();
        tel.seq = s;
        tel.inputRate = r;
        nIn = 0; nextReport = 0; power = 0; nPower = 0;
        built = false;
        stages.clear();
        offPhase = 0;
        if (r >= pagerTuning().minSampleRate - 1) {
            const int d = pickDecimation(r);
            double rr = r;
            for (int f : splitFactors(d)) {
                stages.emplace_back();
                const double out = rr / f;
                const double pass = 38000.0, stop = std::max(pass + 4000.0, out - pass);
                stages.back().design(dmr::lowpassTaps(pass, std::min(stop, rr * 0.5), rr, 60, 9), f);
                rr = out;
            }
            rsPass = std::fabs(rr - kMidRate) < 0.01;
            built = rsPass || rs.configure(rr, kMidRate);
            if (!rsPass) rs.reset();
            fin.design(dmr::lowpassTaps(10000.0, 14000.0, kMidRate, 60, 9), 2);
            design();
        }
        (void)off;
        prev = cf32(0, 0); phase = 0; cur = -1;
        std::fill(ring.begin(), ring.end(), 0.0);
        blkPow = 0; blkN = 0; noiseFloor = 0; snrEma = 0; sigHold = 0; haveFloor = false;
        peakH = -1; locked = false; lockH = -1; hold = 0;
        parser.reset();
        list.clear(); listDirty = true; serial = 0;
        lastLockEnd = -(int64_t)1e12;
        if (built) say("Pagers: listening for POCSAG and FLEX");
    }

    // ---- reports
    void report() {
        tel.seq++;
        tel.timeSec = nowSec();
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        tel.signal = sigHold > 0;
        tel.sync = locked;
        tel.state = (locked || (double)(cur - lastLockEnd) < 2.0 * kWork) ? 1 : 0;
        tel.snrDb = tel.signal ? (float)snrEma : 0.f;
        if (listDirty) {
            tel.messages = std::make_shared<const std::vector<PagerMessage>>(list.begin(), list.end());
            listDirty = false;
        }
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }

    // ---- results
    void emit(std::vector<PagerMessage>& v, int spd, bool flex) {
        for (PagerMessage& pm : v) {
            pm.serial = ++serial;
            pm.timeSec = nowSec();
            pm.speed = spd;
            pm.flex = flex;
            tel.speeds[spd].messages++;
            tel.messagesTotal++;
            tel.dataValid = true;
            list.push_front(pm);
            if (list.size() > kMaxMessages) list.pop_back();
            listDirty = true;
            if (serial <= 40) {
                char b[160];
                snprintf(b, sizeof b, "Pagers: %s %u %s", pagerSpeedName(spd), (unsigned)pm.address, pagerTypeName(pm.type));
                say(b);
            }
        }
        v.clear();
    }
    void heard(int spd) {
        tel.lastSpeed = spd;
        if (spd >= kFlex1600_2) tel.lastFlexSec = nowSec(); else tel.lastPocsagSec = nowSec();
    }
    void addCounts(int spd, int ok, int fixed, int failed) {
        PagerSpeedStat& st = tel.speeds[spd];
        st.ok += (uint64_t)ok; st.fixed += (uint64_t)fixed; st.failed += (uint64_t)failed;
        tel.blocksOk += (uint64_t)(ok + fixed); tel.blocksBad += (uint64_t)failed;
        if (ok + fixed > 0) st.transmissions++;
    }

    // ---- the discriminator and the symbol values
    inline double v(int64_t idx, int win) const {
        return (ring[(size_t)idx & kMask] - ring[(size_t)(idx - win) & kMask]) / win;
    }

    void blockPower() {
        const double p = blkPow / blkN;
        blkPow = 0; blkN = 0;
        if (!haveFloor) { noiseFloor = p; haveFloor = true; }
        else noiseFloor = std::min(p, noiseFloor * 1.0005);
        if (p > 2.0 * noiseFloor) {
            sigHold = 30;
            const double s = 10 * std::log10(std::max(p - noiseFloor, 1e-20) / std::max(noiseFloor, 1e-20));
            snrEma = snrEma == 0 ? s : 0.9 * snrEma + 0.1 * s;
        } else if (sigHold > 0) sigHold--;
    }

    void onSample(cf32 z) {
        blkPow += std::norm(z);
        if (++blkN >= 480) blockPower();
        const cf32 d = z * std::conj(prev);
        if (std::norm(d) > 1e-30f) phase += std::atan2((double)d.imag(), (double)d.real());
        prev = z;
        cur++;
        ring[(size_t)cur & kMask] = phase;
        step();
    }

    // ---- searching
    // POCSAG sync word at baud index h
    Hit evalPocsag(int h) const {
        Hit r;
        const Pat& P = pat[h];
        if (cur < P.off[0] + P.w + 1) return r;
        double x[32], mean = 0;
        const double inv = 1.0 / P.w;
        for (int k = 0; k < 32; k++) {
            const int64_t idx = cur - P.off[k];
            x[k] = (ring[(size_t)idx & kMask] - ring[(size_t)(idx - P.w) & kMask]) * inv;
            mean += x[k];
        }
        mean /= 32;
        uint32_t word = 0;
        for (int k = 0; k < 32; k++) word = (word << 1) | (x[k] < mean ? 1u : 0u);
        const int mism = popcount32(word ^ pager::kPocsagSync);
        int pl;
        if (mism <= 2) pl = 1; else if (mism >= 30) pl = -1; else return r;
        double score = 0;
        for (int k = 0; k < 32; k++) score += ((pager::kPocsagSync >> (31 - k)) & 1 ? -1.0 : 1.0) * (x[k] - mean);
        score *= pl;
        score /= 32;
        if (score < kMinAmp) return r;
        r.ok = true; r.score = score; r.mean = mean; r.pol = pl; r.at = cur;
        return r;
    }

    // FLEX: the marker in the middle of sync 1, then the sync code before it
    Hit evalFlex() const {
        Hit r;
        const Pat& P = flexPat;
        if (cur < P.off[0] + P.w + 16 * 30 + 1) return r;
        double x[32], mean = 0;
        const double inv = 1.0 / P.w;
        for (int k = 0; k < 32; k++) {
            const int64_t idx = cur - P.off[k];
            x[k] = (ring[(size_t)idx & kMask] - ring[(size_t)(idx - P.w) & kMask]) * inv;
            mean += x[k];
        }
        mean /= 32;
        uint32_t word = 0;
        for (int k = 0; k < 32; k++) word = (word << 1) | (x[k] < mean ? 1u : 0u);
        const int mism = popcount32(word ^ pager::kFlexMarker);
        int pl;
        if (mism <= 3) pl = 1; else if (mism >= 29) pl = -1; else return r;
        double score = 0;
        for (int k = 0; k < 32; k++) score += ((pager::kFlexMarker >> (31 - k)) & 1 ? -1.0 : 1.0) * (x[k] - mean);
        score *= pl;
        score /= 32;
        if (score < kMinAmp) return r;
        // the 16 symbols before the marker are the sync code
        uint32_t code = 0;
        for (int j = 0; j < 16; j++) {
            const int64_t idx = cur - (int64_t)(32 + 15 - j) * 30;
            const double xv = v(idx, 30);
            code = (code << 1) | ((pl > 0 ? xv < mean : xv > mean) ? 1u : 0u);
        }
        if (!pager::flexModeForCode((uint16_t)code, 3)) return r;
        r.ok = true; r.score = score; r.mean = mean; r.pol = pl; r.code = (uint16_t)code; r.at = cur;
        return r;
    }

    void step() {
        if (locked) { advance(); return; }
        if (peakH >= 0) {
            const Hit h = peakH == 3 ? evalFlex() : evalPocsag(peakH);
            if (h.ok && h.score > best.score) best = h;
            if (--hold <= 0) { const int hh = peakH; peakH = -1; startLock(hh, best); }
            return;
        }
        if (cur < 4000) return;
        for (int h = 0; h < 4; h++) {
            const Hit r = h == 3 ? evalFlex() : evalPocsag(h);
            if (!r.ok) continue;
            peakH = h; best = r;
            hold = (int)std::ceil((h == 3 ? 30.0 : pat[h].sps) * 0.6);
            return;
        }
    }

    // ---- reading a transmission
    void startLock(int h, const Hit& hit) {
        locked = true; lockH = h; pol = hit.pol;
        m = hit.mean; amp = std::max(hit.score, 0.1);
        tel.cfoHz = m * kWork / (2 * kPi);
        parser.reset();
        wi = bi = 0; curWord = 0; inSync = false;
        stage = 0; cnt = 0; acc = 0;
        lockStart = cur;
        if (h < 3) {
            speed = h == 0 ? kPocsag512 : (h == 1 ? kPocsag1200 : kPocsag2400);
            sps = pat[h].sps; w = pat[h].w;
        } else {
            fmode = pager::flexModeForCode(hit.code, 3);
            speed = fmode->speed;
            sps = 30; w = 30;
        }
        delta = std::max(1, (int)std::lround(sps / 4));
        tNext = (double)hit.at + sps;
    }

    void endLock() {
        if (lockH < 3) {
            parser.flush(tmp);
            emit(tmp, speed, false);
            if (parser.ok + parser.fixed > 0) { addCounts(speed, parser.ok, parser.fixed, parser.failed); heard(speed); }
            parser.ok = parser.fixed = parser.failed = 0;
        }
        locked = false; lockH = -1;
        lastLockEnd = cur;
    }

    void advance() {
        for (;;) {
            const int64_t idx = (int64_t)std::llround(tNext);
            if (idx + delta > cur) return;
            if (idx - delta - w < cur - (int64_t)kRing + 8) { endLock(); return; }        // fell too far behind: lost
            const double x = v(idx, w);
            const double e = (std::fabs(v(idx + delta, w) - m) - std::fabs(v(idx - delta, w) - m)) / std::max(amp, 0.05);
            const double adj = std::max(-1.0, std::min(1.0, kTimingGain * e * sps * 0.5));
            const double tCur = tNext;
            rebased = false;
            symbol(x, tCur);
            if (!locked) return;
            if (!rebased) tNext = tCur + sps + adj;
        }
    }

    void symbol(double x, double tCur) {
        if (lockH < 3) pocsagSymbol(x);
        else flexSymbol(x, tCur);
    }

    inline int bitOf(double x) const { return pol > 0 ? (x < m ? 1 : 0) : (x > m ? 1 : 0); }

    void pocsagSymbol(double x) {
        curWord = (curWord << 1) | (uint32_t)bitOf(x);
        m += 0.004 * (x - m);
        if (++bi < 32) return;
        bi = 0;
        if (inSync) {
            // the batch is over: commit its code words, then the next must start with the sync word
            const bool good = parser.ok + parser.fixed > 0;
            if (good) { addCounts(speed, parser.ok, parser.fixed, parser.failed); heard(speed); }
            parser.ok = parser.fixed = parser.failed = 0;
            if (popcount32(curWord ^ pager::kPocsagSync) <= 4 && good) { inSync = false; wi = 0; }
            else { parser.flush(tmp); emit(tmp, speed, false); locked = false; lockH = -1; lastLockEnd = cur; }
        } else {
            parser.word(curWord, wi, tmp);
            if (!tmp.empty()) emit(tmp, speed, false);
            if (++wi == 16) inSync = true;
        }
    }

    void flexSymbol(double x, double tCur) {
        switch (stage) {
        case 0:   // the complement of the sync code
            acc = (acc << 1) | (uint32_t)bitOf(x);
            if (++cnt == 16) {
                if (popcount32((acc & 0xFFFF) ^ (uint32_t)(uint16_t)~fmode->code) > 4) { endLock(); return; }
                stage = 1; cnt = 0; acc = 0;
            }
            break;
        case 1:   // 16 symbols of dotting
            if (++cnt == 16) { stage = 2; cnt = 0; acc = 0; }
            break;
        case 2: { // the frame information word
            acc = (acc << 1) | (uint32_t)bitOf(x);
            if (++cnt < 32) break;
            const pager::Fixed c = pager::correctWord(acc);
            int cycle, frame;
            if (!c.ok || !pager::flexFiwCheck(pager::rev21(c.data), cycle, frame)) { endLock(); return; }
            // sync 2 follows at the data rate
            sps = kWork / fmode->baud; w = (int)std::lround(sps);
            delta = std::max(1, (int)std::lround(sps / 4));
            tNext = tCur + sps; rebased = true;
            stage = 3; cnt = 0; sx = sa = 0; sn = 0;
            break;
        }
        case 3: { // sync 2: dotting, which gives the centre and the outer amplitude at the data rate
            cnt++;
            if (cnt > 6) { sx += x; sn++; sa += std::fabs(x - m); }
            if (cnt >= fmode->baud * 25 / 1000) {
                if (sn > 4) {
                    const double m2 = sx / sn, a2 = sa / sn;      // a2: mean distance from the old centre, close enough to the outer amplitude
                    if (std::fabs(m2 - m) < 0.35 * amp) m = m2;
                    if (a2 > 0.6 * amp && a2 < 1.5 * amp) amp = a2;
                }
                stage = 4; dataIdx = 0;
                dataTotal = fmode->baud == 1600 ? 2816 : 5632;
                for (auto& b : fb) b.assign(2816, 0);
            }
            break;
        }
        default: { // the data
            const int bidx = dataIdx;
            int a1 = 0, b1 = 0;
            if (fmode->levels == 2) a1 = bitOf(x);
            else {
                const double y = (x - m) / amp * pol;
                if (y < -0.667) { a1 = 1; b1 = 0; } else if (y < 0) { a1 = 1; b1 = 1; } else if (y < 0.667) { a1 = 0; b1 = 1; } else { a1 = 0; b1 = 0; }
            }
            const int spd = fmode->speed;
            if (spd == kFlex1600_2) fb[0][(size_t)bidx] = (uint8_t)a1;
            else if (spd == kFlex3200_4) { fb[0][(size_t)bidx] = (uint8_t)a1; fb[1][(size_t)bidx] = (uint8_t)b1; }
            else if (spd == kFlex3200_2) fb[bidx & 1][(size_t)(bidx >> 1)] = (uint8_t)a1;
            else { const int o = (bidx & 1) * 2; fb[o][(size_t)(bidx >> 1)] = (uint8_t)a1; fb[o + 1][(size_t)(bidx >> 1)] = (uint8_t)b1; }
            if (++dataIdx >= dataTotal) finishFrame();
            break;
        }
        }
    }

    void finishFrame() {
        const int nph = fmode->speed == kFlex1600_2 ? 1 : (fmode->speed == kFlex6400_4 ? 4 : 2);
        pager::FlexPhaseStat st[4];
        std::vector<PagerMessage> got;
        bool any = false;
        int ok = 0, fixed = 0, failed = 0;
        for (int p = 0; p < nph; p++) {
            pager::flexDecodePhase(fb[p].data(), got, st[p]);
            any |= st[p].biwOk;
            ok += st[p].ok; fixed += st[p].fixed; failed += st[p].failed;
        }
        if (any) { addCounts(speed, ok, fixed, failed); heard(speed); }
        emit(got, speed, true);
        locked = false; lockH = -1;
        lastLockEnd = cur;
    }

    // ---- the chain
    void feed(const cf32* x, size_t n) {
        if (!built || n == 0) return;
        for (size_t i = 0; i < n; i++) power += (double)std::norm(x[i]);
        nPower += (int64_t)n;
        const cf32* in = x;
        bool bad = false;
        for (size_t i = 0; i < n; i++) {
            const float re = x[i].real(), im = x[i].imag();
            if (!(std::fabs(re) < 1e4f && std::fabs(im) < 1e4f)) { bad = true; break; }
        }
        if (bad) {
            clean.assign(x, x + n);
            for (auto& s : clean) if (!(std::fabs(s.real()) < 1e4f && std::fabs(s.imag()) < 1e4f)) s = cf32(0, 0);
            in = clean.data();
        }
        double off;
        { std::lock_guard<std::mutex> lk(mu); off = offsetHz; }
        const cf32* c = in;
        size_t cn = n;
        if (off != 0) {
            mixBuf.resize(n);
            const double wm = -2 * kPi * off / curRate;
            const cf32 stp((float)std::cos(wm), (float)std::sin(wm));
            cf32 ph((float)std::cos(offPhase), (float)std::sin(offPhase));
            for (size_t i = 0; i < n; i++) {
                mixBuf[i] = in[i] * ph;
                ph *= stp;
                if ((i & 1023) == 1023) ph /= std::abs(ph);
            }
            offPhase = std::fmod(offPhase + wm * (double)n, 2 * kPi);
            c = mixBuf.data();
        }
        for (size_t s = 0; s < stages.size(); s++) {
            std::vector<cf32>& dst = (s & 1) ? bufB : bufA;
            dst.clear();
            stages[s].process(c, cn, dst);
            c = dst.data(); cn = dst.size();
        }
        if (!rsPass) {
            rsOut.clear();
            rs.process(c, cn, rsOut);
            c = rsOut.data(); cn = rsOut.size();
        }
        finOut.clear();
        fin.process(c, cn, finOut);
        // the report instants are in input samples; the decoding runs a little ahead of them
        nIn += (int64_t)n;
        for (const cf32& z : finOut) onSample(z);
        while ((double)nIn >= nextReport) {
            nextReport += 0.25 * curRate;
            report();
        }
    }
};

PagerReceiver::PagerReceiver() : p_(std::make_unique<Impl>()) {}
PagerReceiver::~PagerReceiver() = default;

// configure() and reset() may come from another thread than feed(): they only leave a request that feed() carries out
void PagerReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rate = inputRateHz;
    p_->resetReq = true;
}
void PagerReceiver::setSignalOffset(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->offsetHz = hz;
    p_->resetReq = true;
}
bool PagerReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rate >= pagerTuning().minSampleRate - 1;
}
void PagerReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetReq = true;
    const uint64_t s = p_->pub.seq;
    p_->pub = PagerTelemetry();
    p_->pub.seq = s;
}

void PagerReceiver::feed(const cf32* x, size_t n) {
    Impl& im = *p_;
    if (im.resetReq.exchange(false)) im.resetState();
    if (im.curRate <= 0) return;
    im.feed(x, n);
}

bool PagerReceiver::telemetry(PagerTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void PagerReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

ModeTuning pagerTuning() {
    ModeTuning t;
    t.stdMode = 25; t.id = "pager"; t.name = "Pagers";
    t.minMhz = 25; t.maxMhz = 1000; t.defMhz = 466.075;
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.025;
    t.minSampleRate = 1000000;
    t.tuneOffsetHz = 50000;                  // the radio sits 50 kHz above the channel, away from its DC spike (as Inmarsat-C and Aero)
    return t;
}

} // namespace dect2
