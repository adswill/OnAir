// Argos receiver (see argos_rx.h).
//
// Front end: the stream is mixed to the dial offset and averaged down to 128 kHz.
// Uplink burst finder (as the COSPAS-SARSAT one): Hann-windowed 4096 point FFTs (32 ms) searched for one line well above the median bin that
// was not there a moment ago; a burst runs from the first block with a line until 6 blocks without one. Its carrier is the median of the first
// lines. The burst is mixed to 0 Hz, averaged to 32 kHz and demodulated: each bit is two half-bit vector sums, and the angle between them is
// +2.2 rad for a 1 and -2.2 rad for a 0 whatever the carrier phase (Manchester phase modulation of +-1.1 rad). The first bit is where the
// carrier before it ends and the steps of +-2.2 rad begin; the timing and the bit rate (+-0.5 %) are searched for the best fit of both.
// Downlink: the 128 kHz stream is averaged to 64 kHz; the carrier is found as the line of the squared signal (a BPSK or PM signal squared has
// a line at twice its carrier) within +-14 kHz, then followed by a second-order squaring loop (60 Hz) through the pass's Doppler. Per 0.1 s
// the residual carrier is taken off and the data axis found; bits are the difference of the two half-bit sums along it, timed by the
// alignment where those differences are largest.
#include "dect2/argos/argos_rx.h"
#include "dect2/argos/argos_tel.h"
#include "dect2/common/mathconst.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <ctime>
#include <deque>
#include <mutex>

namespace dect2 {

using dect2::math::kPi;
using cd = std::complex<double>;

ModeTuning argosTuning() {
    ModeTuning t;
    t.stdMode = 154;
    t.id = "argos";
    t.name = "Argos";
    t.minMhz = 401.5;
    t.maxMhz = 466.1;
    t.defMhz = argos::kUplinkMhz;
    t.sampleRate = 256000;
    t.basebandHz = 16000;
    t.bandwidthMhz = 0.1;
    t.minSampleRate = 128000;
    t.tuneOffsetHz = 20000;
    return t;
}

namespace argos {

namespace {

double closeness(double a) { return std::cos(std::fabs(a) - 2 * kPhaseDevRad); }

struct Halves {
    std::vector<cd> P;
    size_t n = 0;
    // the angle from the first half of the bit starting at b0 to its second half; false outside the burst
    bool step(double b0, double spb, double& ang, double& mag) const {
        const double b1 = b0 + spb;
        if (b0 < 0 || b1 > (double)n) return false;
        const size_t i0 = (size_t)std::lround(b0), im = (size_t)std::lround(b0 + spb * 0.5), i1 = std::min(n, (size_t)std::lround(b1));
        const cd h1 = P[im] - P[i0], h2 = P[i1] - P[im];
        const cd c = h1 * std::conj(h2);
        mag = std::abs(h1) * std::abs(h2);
        ang = mag > 1e-18 ? std::arg(c) : 0.0;
        return true;
    }
};

}  // namespace

static bool demodCore(const std::vector<cf32>& x, DemodResult& out, long hintStart, bool forceLow, bool legacy) {
    out = DemodResult();
    const size_t n = x.size();
    if (n < (size_t)(kFs * 0.3)) return false;
    Halves h;
    h.n = n;
    h.P.assign(n + 1, cd(0, 0));
    for (size_t i = 0; i < n; i++) h.P[i + 1] = h.P[i] + cd(x[i].real(), x[i].imag());
    // the burst's extent from its power (1 ms windows)
    const size_t W = 32;
    std::vector<double> env(n / W);
    // 4.0: the carrier is at 0 Hz here, so a 1 ms window is summed coherently (|sum|^2 / W): the noise power per window stays that of one sample while
    // the carrier grows with W, which is 15 dB more contrast than the mean power of the 32 kHz stream (the old gate lost bursts below about 5 dB in 32 kHz)
    if (legacy) for (size_t k = 0; k < env.size(); k++) { double s = 0; for (size_t i = k * W; i < (k + 1) * W; i++) s += std::norm(x[i]); env[k] = s / W; }
    else for (size_t k = 0; k < env.size(); k++) env[k] = std::norm(h.P[(k + 1) * W] - h.P[k * W]) / (double)W;
    // 4 windows are averaged (a chi-square of 8 degrees of freedom in noise); the 5th percentile of that is 0.37 of the noise mean
    if (!legacy) {
        std::vector<double> sm(env.size());
        for (size_t k = 0; k < env.size(); k++) { double a = 0; int c = 0; for (long j = (long)k - 1; j <= (long)k + 2; j++) if (j >= 0 && j < (long)env.size()) { a += env[(size_t)j]; c++; } sm[k] = a / c; }
        env = sm;
    }
    std::vector<double> srt = env;
    std::sort(srt.begin(), srt.end());
    const double ref = srt[srt.size() * 7 / 10], floorP = srt[srt.size() / 20] / (legacy ? 1.0 : 0.37);
    // 4.0: where the power gate cannot see the burst (the carrier is less than 4 x the noise in a 1 ms window: below about 3 dB in 5 kHz) the extent comes
    // from the receiver's line detector (hintStart), the message runs to the end of x and its length is the one of the 8 whose bits fit best
    bool lowSnr = false;
    double start = 0, end = (double)n;
    if (forceLow || ref < 4 * floorP) {
        if (hintStart < 0) return false;
        lowSnr = true;
        start = (double)hintStart;
    } else {
        const double gate = floorP + 0.3 * (ref - floorP);
        size_t s0 = 0, s1 = env.size();
        while (s0 < env.size() && env[s0] < gate) s0++;
        while (s1 > s0 && env[s1 - 1] < gate) s1--;
        if (s1 <= s0 + 10) return false;
        start = (double)(s0 * W); end = (double)(s1 * W);
    }
    // the carrier section: 2.5 ms sums keep their size while the carrier is bare; Manchester (or any) modulation shrinks them
    {
        const size_t B = 80;
        std::vector<double> a;
        for (size_t i = (size_t)start; i + B <= (size_t)end; i += B) a.push_back(std::abs(h.P[i + B] - h.P[i]));
        if (a.size() >= 24) {
            std::vector<double> first(a.begin() + 2, a.begin() + 20);
            std::sort(first.begin(), first.end());
            const double r = first[first.size() / 2];
            size_t k = 20;
            while (k + 2 < a.size() && !(a[k] < 0.65 * r && a[k + 1] < 0.65 * r && a[k + 2] < 0.65 * r)) k++;
            out.carrierMs = (float)((double)k * B / kFs * 1000.0);
        }
    }
    // timing and rate: the 24 bits after t0 step by +-2.2 rad, the 12 bit periods before do not (bare carrier)
    double bestS = -1e9, bestT = 0, bestSpb = kFs / kBitRate;
    const double rates[] = {-0.005, -0.0025, 0.0, 0.0025, 0.005};
    for (double r : rates) {
        const double spb = kFs / kBitRate * (1 + r);
        const double lo = std::max(spb * 12, start + 0.09 * kFs), hi = std::min(end - 30 * spb, start + 0.26 * kFs);
        for (double t0 = lo; t0 <= hi; t0 += 1) {
            double sc = 0, a, m;
            bool ok = true;
            for (int k = 0; k < 24 && ok; k++) { ok = h.step(t0 + k * spb, spb, a, m); sc += closeness(a); }
            for (int k = 1; k <= 12 && ok; k++) { ok = h.step(t0 - k * spb, spb, a, m); sc -= closeness(a); }
            if (ok && sc > bestS) { bestS = sc; bestT = t0; bestSpb = spb; }
        }
    }
    if (bestS <= -1e8) { out.kind = out.carrierMs >= 100 ? 2 : 0; return false; }
    // the bit rate over the whole message: 24 bits cannot tell +-0.25 % apart, 300 bits can (a quarter of a bit at the end)
    {
        const int nb = std::min(lowSnr ? kMinBits + 32 : 1000000, (int)((end - bestT) / bestSpb) - 1);
        if (nb > 24) {
            double bs = -1e9, bt = bestT, bp = bestSpb;
            for (int ri = -12; ri <= 12; ri++) {
                const double spb = kFs / kBitRate * (1 + 0.0005 * ri);
                for (double dt = -4; dt <= 4; dt += 0.5) {
                    double sc = 0, a, m;
                    bool ok = true;
                    for (int k = 0; k < nb && ok; k++) { ok = h.step(bestT + dt + k * spb, spb, a, m); sc += closeness(a); }
                    if (ok && sc > bs) { bs = sc; bt = bestT + dt; bp = spb; }
                }
            }
            bestT = bt; bestSpb = bp;
        }
    }
    if (lowSnr) {
        // every bit to the end of x; the length is where the running sum of (closeness - 0.75) peaks over the lengths that exist: bits of noise score about
        // 0.51 and pull it down, bits of the message about 0.7 to 1
        std::vector<uint8_t> all;
        std::vector<double> cs;
        for (double b0 = bestT; b0 + bestSpb <= (double)n; b0 += bestSpb) {
            double a, m;
            if (!h.step(b0, bestSpb, a, m)) break;
            all.push_back(a > 0 ? 1 : 0);
            cs.push_back(closeness(a));
        }
        double run = 0, bestRun = -1e9, qSum = 0, qBest = 0;
        int L = 0;
        for (size_t k = 0; k < cs.size(); k++) {
            run += cs[k] - 0.75; qSum += cs[k];
            const int cnt = (int)k + 1;
            if (cnt >= kMinBits && cnt <= kMaxBits && (cnt - kMinBits) % 32 == 0 && run > bestRun) { bestRun = run; L = cnt; qBest = qSum / cnt; }
        }
        if (L == 0 || qBest < 0.72) return false;
        all.resize((size_t)L);
        out.kind = 1;
        out.carrierMs = (float)((bestT - start) / kFs * 1000.0);
        out.quality = (float)qBest;
        out.bits = all;
        return true;
    }
    // the bits, to the end of the burst
    std::vector<uint8_t> bits;
    double q = 0;
    for (double b0 = bestT; b0 + bestSpb <= end + 0.3 * bestSpb; b0 += bestSpb) {
        double a, m;
        if (!h.step(b0, bestSpb, a, m)) break;
        bits.push_back(a > 0 ? 1 : 0);
        q += closeness(a);
    }
    out.quality = bits.empty() ? 0.f : (float)(q / (double)bits.size());
    const double carrierMs = (bestT - start) / kFs * 1000.0;
    const int L = nearestA2Length((int)bits.size());
    if (out.quality >= 0.5f && carrierMs >= 120 && carrierMs <= 210 && L > 0) {
        bits.resize((size_t)L, 0);
        out.kind = 1;
        out.carrierMs = (float)carrierMs;
        out.bits = bits;
        return true;
    }
    out.kind = out.carrierMs >= 100 ? 2 : 0;
    return false;
}

bool demodUplinkBurst(const std::vector<cf32>& x, DemodResult& out, long hintStart, bool legacy) {
    DemodResult r;
    if (demodCore(x, r, hintStart, false, legacy)) { out = r; return true; }
    DemodResult r2;
    if (!legacy && hintStart >= 0 && demodCore(x, r2, hintStart, true, false)) { out = r2; return true; }
    out = r;
    return false;
}

}  // namespace argos

struct ArgosReceiver::Impl {
    std::mutex mu;
    ArgosTelemetry tel;
    std::function<void(const std::string&)> log;
    double fs = 0, offHz = 0, dialHz = 0;
    int linkSet = 0, link = 0;
    bool rateSet = false;
    bool lowSnr = true;   // the 4.0 weak-signal path (docs/SENSITIVITY-4.0-B2.md); false: the 3.9 receiver

    // mixer and the average down to 128 kHz
    static constexpr double kDet = 128000.0;
    double loRe = 1, loIm = 0, stRe = 1, stIm = 0, accRe = 0, accIm = 0, binEnd = 0;
    int loCount = 0, accN = 0;
    uint64_t nIn = 0, k128 = 0;
    double pwAcc = 0; uint64_t pwN = 0;

    // ---- uplink: the burst finder at 128 kHz
    static constexpr size_t kRing = 262144, kBlock = 4096;
    std::vector<cf32> ring = std::vector<cf32>(kRing);
    uint64_t n128 = 0;
    std::vector<float> win = std::vector<float>(kBlock);
    Fft fft{(int)kBlock};
    std::vector<float> bg = std::vector<float>(kBlock, 0.f);
    int warm = 0;
    bool active = false;
    int miss = 0, dets = 0;
    static constexpr float kWeakLine = 8.f;
    int candRun = 0; size_t candPk = 0; uint64_t candStart = 0; std::vector<double> candPeaks;
    uint64_t firstBlockStart = 0;
    std::vector<double> peaks;
    float peakSnr = -200, peakLevel = -200;
    double lastDecodeSec = -1e9, lastPublish = 0;

    // ---- downlink at 64 kHz
    static constexpr double kDl = 64000.0;
    static constexpr int kAcqN = 8192, kAcqAvg = 4;
    static constexpr int kSpb = 160;   // 400 bit/s at 64 kHz
    static constexpr double kLoopHz = 60.0;   // the carrier loop: follows a LEO pass (up to about 150 Hz/s at 466 MHz)
    cf32 half{0, 0}; bool haveHalf = false;
    uint64_t n64 = 0;
    bool locked = false;
    std::vector<cf32> acqBuf;
    std::vector<float> acqMag;
    int acqCount = 0;
    Fft acqFft{kAcqN};
    std::vector<float> acqWin;
    double th = 0, om = 0;           // loop phase and rad/sample
    cd cohSum{0, 0}; double cohMag = 0; int cohN = 0, badWin = 0;
    double lastDopSec = -1;
    std::vector<cd> blk;             // loop output waiting for the 0.1 s block
    double alphaPrev = 0; bool haveAlpha = false;
    std::deque<float> sb;            // data-axis samples
    uint64_t sbBase = 0;             // absolute index of sb[0]
    double bitPos = -1;              // absolute index of the next bit's start
    int prevBit = 0;
    argos::HdlcDeframer dfr[3];

    Impl() {
        for (size_t i = 0; i < kBlock; i++) win[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)kBlock));
        acqWin.resize(kAcqN);
        for (int i = 0; i < kAcqN; i++) acqWin[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)kAcqN));
    }

    void chooseLink() {
        const int l = linkSet == 1 ? 0 : linkSet == 2 ? 1 : (dialHz > 430e6 ? 1 : 0);
        if (l != link) { link = l; resetState(); }
        tel.link = link; tel.linkSetting = linkSet; tel.dialHz = dialHz;
    }

    void resetState() {
        loRe = 1; loIm = 0;
        const double w = -2 * kPi * offHz / (fs > 0 ? fs : 1);
        stRe = std::cos(w); stIm = std::sin(w);
        accRe = accIm = 0; accN = 0; nIn = 0; k128 = 0;
        binEnd = fs > 0 ? fs / kDet : 0;
        n128 = 0;
        std::fill(bg.begin(), bg.end(), 0.f);
        warm = 0; active = false; miss = 0; dets = 0; peaks.clear(); candRun = 0;
        lastDecodeSec = -1e9; lastPublish = 0;
        haveHalf = false; n64 = 0; unlock();
        for (auto& d : dfr) d = argos::HdlcDeframer();
    }

    void unlock() {
        locked = false;
        acqBuf.clear(); acqMag.assign(kAcqN, 0.f); acqCount = 0;
        blk.clear(); sb.clear(); sbBase = n64; bitPos = -1; haveAlpha = false;
        cohSum = 0; cohMag = 0; cohN = 0; badWin = 0;
        tel.dlLocked = false;
    }

    double timeSec() const { return (double)k128 / kDet; }

    void publish() {
        tel.timeSec = timeSec();
        tel.state = rateSet ? (timeSec() - lastDecodeSec < 300 ? 2 : 1) : 0;
        if (pwN) tel.levelDb = (float)(10 * std::log10(pwAcc / (double)pwN + 1e-20));
        if (link == 1) { pwAcc = 0; pwN = 0; }
        tel.seq++;
    }

    // ---------------------------------------------------------------- uplink

    void addPtt(const ArgosBurst& e) {
        auto it = std::find_if(tel.ptts.begin(), tel.ptts.end(), [&](const ArgosPtt& p) { return p.id == e.msg.id; });
        ArgosPtt p;
        if (it != tel.ptts.end()) { p = *it; tel.ptts.erase(it); } else { p.id = e.msg.id; p.idHex = e.msg.idHex; p.firstWall = e.wallTime; }
        p.count++;
        p.lastWall = e.wallTime;
        p.lastData = argos::hexOf(e.msg.data);
        p.freq.push_back({e.timeSec, e.carrierHz});
        if (p.freq.size() > kArgosFreqMax) p.freq.erase(p.freq.begin());
        p.driftHzPerMin = 0;
        if (p.freq.size() >= 2) {
            double st = 0, sf = 0, stt = 0, stf = 0;
            const double k = (double)p.freq.size();
            for (const auto& [t, f] : p.freq) { st += t; sf += f; stt += t * t; stf += t * f; }
            const double den = k * stt - st * st;
            if (den > 1e-9) p.driftHzPerMin = 60.0 * (k * stf - st * sf) / den;
        }
        tel.ptts.insert(tel.ptts.begin(), p);
        if (tel.ptts.size() > kArgosPttMax) tel.ptts.pop_back();
    }

    void finishBurst() {
        active = false;
        if (dets >= 4 && peakSnr >= 10.f) {
            tel.bursts++;
            const uint64_t start = firstBlockStart >= 8192 ? firstBlockStart - 8192 : 0;
            const uint64_t from = n128 > kRing - 4096 && start < n128 - (kRing - 4096) ? n128 - (kRing - 4096) : start;
            std::vector<double> pk(peaks.begin(), peaks.begin() + std::min<size_t>(4, peaks.size()));
            std::sort(pk.begin(), pk.end());
            const double carrier = pk[pk.size() / 2];
            std::vector<cf32> y;
            y.reserve((size_t)(n128 - from) / 4 + 1);
            {
                const double w = -2 * kPi * carrier / kDet;
                cd lo(1, 0);
                const cd st(std::cos(w), std::sin(w));
                cd acc(0, 0);
                for (uint64_t i = from, j = 0; i < n128; i++, j++) {
                    const cf32 v = ring[(size_t)(i & (kRing - 1))];
                    acc += cd(v.real(), v.imag()) * lo;
                    lo *= st;
                    if ((j & 1023) == 1023) lo /= std::abs(lo);
                    if ((j & 3) == 3) { y.push_back(cf32((float)(acc.real() * 0.25), (float)(acc.imag() * 0.25))); acc = 0; }
                }
            }
            argos::DemodResult r;
            const bool got = argos::demodUplinkBurst(y, r, lowSnr ? (long)((firstBlockStart - from) / 4) : -1L, !lowSnr);
            ArgosBurst e;
            e.wallTime = (int64_t)std::time(nullptr);
            e.timeSec = (double)from / kDet;
            e.carrierHz = carrier;
            e.snrDb = peakSnr; e.levelDb = peakLevel;
            e.durationMs = (float)((double)(n128 - firstBlockStart) / kDet * 1000.0 - 6 * 32);
            e.carrierMs = r.carrierMs;
            e.quality = r.quality;
            if (got && argos::parseA2(r.bits, e.msg)) {
                e.kind = 1;
                tel.blocksOk++;
                tel.dataValid = true;
                lastDecodeSec = timeSec();
                addPtt(e);
                if (log) {
                    char line[256];
                    snprintf(line, sizeof line, "Argos PTT %s: %d data bits %s, %+.0f Hz", e.msg.idHex.c_str(), e.msg.dataBits, argos::hexOf(e.msg.data).c_str(), carrier);
                    log(line);
                }
            } else if (r.kind == 2) {
                e.kind = 2;
                tel.otherBursts++;
                tel.blocksBad++;
            } else {
                tel.blocksBad++;
            }
            if (e.kind) {
                tel.log.insert(tel.log.begin(), e);
                if (tel.log.size() > kArgosLogMax) tel.log.pop_back();
            }
            tel.cfoHz = carrier; tel.snrDb = peakSnr;
        }
        peaks.clear(); dets = 0; peakSnr = -200; peakLevel = -200;
        publish();
    }

    void block() {
        const uint64_t base = n128 - kBlock;
        std::vector<cf32> buf(kBlock);
        double pw = 0;
        for (size_t i = 0; i < kBlock; i++) {
            const cf32 v = ring[(size_t)((base + i) & (kRing - 1))];
            pw += (double)v.real() * v.real() + (double)v.imag() * v.imag();
            buf[i] = v * win[i];
        }
        fft.forward(buf.data());
        std::vector<float> p(kBlock);
        for (size_t k = 0; k < kBlock; k++) p[k] = buf[k].real() * buf[k].real() + buf[k].imag() * buf[k].imag();
        const size_t kMax = (size_t)(46000.0 / kDet * (double)kBlock);
        auto allowed = [&](size_t k) { return k <= kMax || k >= kBlock - kMax; };
        std::vector<float> sel;
        sel.reserve(kBlock);
        for (size_t k = 0; k < kBlock; k++) if (allowed(k)) sel.push_back(p[k]);
        std::nth_element(sel.begin(), sel.begin() + (long)(sel.size() / 2), sel.end());
        const float med = std::max(sel[sel.size() / 2], 1e-20f);
        size_t pk = 0;
        float pmax = 0;
        for (size_t k = 0; k < kBlock; k++) if (allowed(k) && p[k] > pmax && p[k] > 6.f * bg[k]) { pmax = p[k]; pk = k; }
        // 4.0: a weaker line (12 x the median bin) counts when the same bin (+-2) holds one in 3 blocks in a row; one block alone needs 30 x as before
        const bool strong = warm >= 12 && pmax >= 30.f * med;
        bool det = strong;
        if (!strong && lowSnr && warm >= 12 && pmax >= kWeakLine * med) {
            const long dk = std::labs((long)pk - (long)candPk), wrap = (long)kBlock - dk;
            if (candRun > 0 && std::min(dk, wrap) <= 2) candRun++; else { candRun = 1; candStart = base; candPeaks.clear(); }
            candPk = pk;
            candPeaks.push_back(0);   // the frequency is filled in below
            bool nearBurst = false;
            if (active && !peaks.empty()) { const double db = std::fabs((double)(pk < kBlock / 2 ? (long)pk : (long)pk - (long)kBlock) - peaks.back() / kDet * (double)kBlock); nearBurst = db <= 3.0; }
            if (candRun >= 3 || nearBurst) det = true;
        } else if (!strong) {
            candRun = 0;
        }
        float snr = -200;
        double f = 0;
        if (det) {
            const size_t a = (pk + kBlock - 1) % kBlock, c = (pk + 1) % kBlock;
            const double la = std::log((double)p[a] + 1e-30), lb = std::log((double)p[pk] + 1e-30), lc = std::log((double)p[c] + 1e-30);
            const double den = la - 2 * lb + lc;
            const double delta = std::fabs(den) > 1e-9 ? 0.5 * (la - lc) / den : 0.0;
            const double bin = (double)(pk < kBlock / 2 ? (long)pk : (long)pk - (long)kBlock) + std::max(-1.0, std::min(1.0, delta));
            f = bin * kDet / (double)kBlock;
            const float nm = med / 0.6931f;
            const float s3 = p[a] + p[pk] + p[c];
            snr = 10.f * std::log10(std::max(1e-3f, (s3 - 3 * nm) / (1.5f * nm)));
        }
        if (det && !strong && candRun >= 3) {   // the earlier blocks of the run belong to the burst
            if (!active) { active = true; firstBlockStart = candStart; miss = 0; dets = (int)candRun - 1; peakSnr = -200; peakLevel = -200; peaks.clear(); for (int i = 0; i < candRun - 1; i++) peaks.push_back(f); }
        }
        if (det) {
            if (!active) { active = true; firstBlockStart = base; miss = 0; dets = 0; peakSnr = -200; peakLevel = -200; peaks.clear(); }
            miss = 0; dets++;
            peaks.push_back(f);
            peakSnr = std::max(peakSnr, snr);
            peakLevel = std::max(peakLevel, 10.f * std::log10((float)(pw / kBlock) + 1e-12f));
        } else if (active) {
            miss++;
        }
        if (active && (miss >= 6 || n128 - firstBlockStart > (uint64_t)(1.3 * kDet))) finishBurst();
        if (!active && !det) {
            for (size_t k = 0; k < kBlock; k++) bg[k] = warm == 0 ? std::min(p[k], 4.f * med) : bg[k] * 0.98f + std::min(p[k], 4.f * med) * 0.02f;
            if (warm < 1000) warm++;
        }
        if (!active) tel.levelDb = 10.f * std::log10((float)(pw / kBlock) + 1e-12f);
    }

    // ---------------------------------------------------------------- downlink

    void acquire(cf32 v) {
        acqBuf.push_back(v);
        if ((int)acqBuf.size() < kAcqN) return;
        std::vector<cf32> z(kAcqN);
        for (int i = 0; i < kAcqN; i++) z[(size_t)i] = acqBuf[(size_t)i] * acqBuf[(size_t)i] * acqWin[(size_t)i];
        acqFft.forward(z.data());
        for (int k = 0; k < kAcqN; k++) acqMag[(size_t)k] += std::norm(z[(size_t)k]);
        acqBuf.clear();
        if (++acqCount < kAcqAvg) return;
        acqCount = 0;
        // twice the carrier within +-28 kHz (the +-14 kHz of Doppler and radio offset)
        const int kMax = (int)(28000.0 / kDl * kAcqN);
        std::vector<float> sel;
        int best = 0;
        float bm = 0;
        for (int d = -kMax; d <= kMax; d++) {
            const float m = acqMag[(size_t)((d + kAcqN) % kAcqN)];
            sel.push_back(m);
            if (m > bm) { bm = m; best = d; }
        }
        std::nth_element(sel.begin(), sel.begin() + (long)(sel.size() / 2), sel.end());
        const float med = std::max(1e-30f, sel[sel.size() / 2]);
        tel.dlCnrDb = 10.f * std::log10(bm / med);
        if (bm >= 40.f * med) {
            const float a = acqMag[(size_t)((best - 1 + kAcqN) % kAcqN)], c = acqMag[(size_t)((best + 1 + kAcqN) % kAcqN)];
            const double la = std::log(a + 1e-30), lb = std::log(bm + 1e-30), lc = std::log(c + 1e-30);
            const double den = la - 2 * lb + lc;
            const double delta = std::fabs(den) > 1e-9 ? 0.5 * (la - lc) / den : 0.0;
            const double f2 = ((double)best + std::max(-1.0, std::min(1.0, delta))) * kDl / kAcqN;
            om = 2 * kPi * (f2 / 2) / kDl;
            th = 0;
            locked = true;
            tel.dlLocked = true;
            blk.clear(); sb.clear(); sbBase = n64; bitPos = -1; haveAlpha = false;
            cohSum = 0; cohMag = 0; cohN = 0; badWin = 0;
        }
        std::fill(acqMag.begin(), acqMag.end(), 0.f);
    }

    void dataBlock() {
        // residual carrier off, then the axis of the remaining (antipodal) data
        cd m(0, 0);
        for (const cd& v : blk) m += v;
        m /= (double)blk.size();
        cd s2(0, 0);
        for (const cd& v : blk) { const cd z = v - m; s2 += z * z; }
        double alpha = 0.5 * std::arg(s2);
        if (haveAlpha && std::cos(alpha - alphaPrev) < 0) alpha += kPi;
        alphaPrev = alpha; haveAlpha = true;
        const cd rot = std::polar(1.0, -alpha);
        for (const cd& v : blk) sb.push_back((float)((v - m) * rot).real());
        blk.clear();
        // bit timing over what is buffered: the alignment with the largest half-bit differences
        const size_t N = sb.size();
        if (N < (size_t)(kSpb * 20)) return;
        std::vector<double> P(N + 1, 0.0);
        for (size_t i = 0; i < N; i++) P[i + 1] = P[i] + sb[i];
        const size_t look = std::min<size_t>(N, (size_t)kSpb * 40);
        const size_t o = N - look;
        double bestM = -1, secondM = -1;
        int bestTau = 0;
        std::vector<double> M(kSpb, 0.0);
        for (int tau = 0; tau < kSpb; tau++) {
            double s = 0;
            for (size_t k = o + (size_t)tau; k + kSpb <= N; k += kSpb) s += std::fabs((P[k + kSpb / 2] - P[k]) - (P[k + kSpb] - P[k + kSpb / 2]));
            M[(size_t)tau] = s;
            if (s > bestM) { bestM = s; bestTau = tau; }
        }
        secondM = M[(size_t)((bestTau + kSpb / 2) % kSpb)];
        const double target = (double)(sbBase + o + (size_t)bestTau);
        if (bitPos < 0) {
            bitPos = target;
            while (bitPos - (double)kSpb >= (double)sbBase) bitPos -= kSpb;
        } else if (bestM > 1.2 * secondM) {
            double e = std::fmod(target - bitPos, (double)kSpb);
            if (e > kSpb / 2) e -= kSpb; else if (e < -kSpb / 2) e += kSpb;
            bitPos += 0.3 * e;
        }
        // bits
        while (bitPos + kSpb <= (double)(sbBase + N)) {
            const size_t i0 = (size_t)std::llround(bitPos) - sbBase;
            if (i0 + kSpb > N) break;
            const double d = (P[i0 + kSpb / 2] - P[i0]) - (P[i0 + kSpb] - P[i0 + kSpb / 2]);
            const int bit = d > 0 ? 1 : 0;
            bitPos += kSpb;
            tel.dlBits++;
            const int in[3] = {bit, bit ^ 1, bit == prevBit ? 1 : 0};
            prevBit = bit;
            for (int c = 0; c < 3; c++) {
                if (!dfr[c].push(in[c])) continue;
                const argos::HdlcFrame& f = dfr[c].frame();
                tel.dlFlags[c] = dfr[c].flags;
                const bool steady = dfr[c].flags > 20 && dfr[c].flags > 4 * std::max(dfr[(c + 1) % 3].flags, dfr[(c + 2) % 3].flags);
                if (!f.fcsOk) { tel.dlFcsBad++; if (!steady || f.bytes.size() < 6) continue; }
                ArgosDlFrame fr;
                fr.wallTime = (int64_t)std::time(nullptr);
                fr.timeSec = (double)n64 / kDl;
                fr.coding = c; fr.fcsOk = f.fcsOk;
                fr.hex = argos::hexOf(f.bytes);
                fr.bytes = (int)f.bytes.size();
                tel.frames.insert(tel.frames.begin(), fr);
                if (tel.frames.size() > kArgosDlMax) tel.frames.pop_back();
                if (f.fcsOk) {
                    tel.blocksOk++;
                    tel.dataValid = true;
                    lastDecodeSec = timeSec();
                    if (log) { char line[200]; snprintf(line, sizeof line, "Argos downlink frame, %d bytes", fr.bytes); log(line); }
                }
            }
        }
        for (int c = 0; c < 3; c++) tel.dlFlags[c] = dfr[c].flags;
        {
            int bc = -1; uint64_t bf = 0, other = 0;
            for (int c = 0; c < 3; c++) if (dfr[c].flags > bf) { bf = dfr[c].flags; bc = c; }
            for (int c = 0; c < 3; c++) if (c != bc) other = std::max(other, dfr[c].flags);
            tel.dlCoding = bf > 10 && bf > 3 * other ? bc : -1;
        }
        // keep one bit of history before the next bit
        const double keepFrom = bitPos - kSpb;
        while (!sb.empty() && (double)sbBase < keepFrom) { sb.pop_front(); sbBase++; }
    }

    void sample64(cf32 v) {
        n64++;
        if (!locked) { acquire(v); return; }
        // squaring loop: the square of the rotated sample is a line at 0 (BPSK) or at pi (phase modulation of +-1.1 rad) when locked
        const cd y(v.real(), v.imag());
        const cd r = y * std::polar(1.0, -th);
        const cd q = r * r;
        const double a = std::abs(q);
        const double e = a > 1e-30 ? q.imag() / a : 0.0;
        const double wn = 2 * kLoopHz / (0.707 + 1 / (4 * 0.707)), T = 1 / kDl;
        const double k1 = 2 * 0.707 * wn * T, k2 = (wn * T) * (wn * T);
        th += om + 0.5 * k1 * e;
        om += 0.5 * k2 * e;
        if (th > kPi) th -= 2 * kPi; else if (th < -kPi) th += 2 * kPi;
        cohSum += q; cohMag += a; cohN++;
        if (cohN >= (int)(0.25 * kDl)) {
            const double coh = cohMag > 0 ? std::abs(cohSum) / cohMag : 0;
            badWin = coh < 0.15 ? badWin + 1 : 0;
            cohSum = 0; cohMag = 0; cohN = 0;
            if (badWin >= 4) { unlock(); return; }
        }
        const double hz = om * kDl / (2 * kPi);
        tel.dlCarrierHz = hz;
        tel.cfoHz = hz;
        const double now = (double)n64 / kDl;
        if (now - lastDopSec >= 0.5) {
            lastDopSec = now;
            struct timespec ts {};
            clock_gettime(CLOCK_REALTIME, &ts);   // timespec_get is missing in the Android NDK at API 28
            tel.doppler.push_back({(double)ts.tv_sec + ts.tv_nsec * 1e-9, hz});
            if (tel.doppler.size() > kArgosDopplerMax) tel.doppler.erase(tel.doppler.begin());
        }
        blk.push_back(r);
        if (blk.size() >= (size_t)(0.1 * kDl)) dataBlock();
    }

    void sample128(double re, double im) {
        pwAcc += re * re + im * im; pwN++;
        if (link == 0) {
            ring[(size_t)(n128 & (kRing - 1))] = cf32((float)re, (float)im);
            n128++;
            if (n128 % kBlock == 0) block();
            return;
        }
        if (!haveHalf) { half = cf32((float)re, (float)im); haveHalf = true; return; }
        haveHalf = false;
        sample64(cf32((half.real() + (float)re) * 0.5f, (half.imag() + (float)im) * 0.5f));
    }
};

ArgosReceiver::ArgosReceiver() : p_(std::make_unique<Impl>()) {}
ArgosReceiver::~ArgosReceiver() = default;

void ArgosReceiver::configure(double r) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->fs = r;
    p_->rateSet = r >= argosTuning().minSampleRate - 1;
    p_->resetState();
    p_->chooseLink();
    p_->tel.inputRate = r;
    p_->tel.state = p_->rateSet ? 1 : 0;
    p_->tel.seq++;
}
void ArgosReceiver::setSignalOffset(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (hz == p_->offHz) return;
    p_->offHz = hz;
    const double w = -2 * kPi * hz / (p_->fs > 0 ? p_->fs : 1);
    p_->stRe = std::cos(w); p_->stIm = std::sin(w);
}
void ArgosReceiver::setDialHz(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->dialHz = hz;
    p_->chooseLink();
}
void ArgosReceiver::setLink(int l) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->linkSet = std::max(0, std::min(2, l));
    p_->chooseLink();
    p_->tel.seq++;
}
void ArgosReceiver::setWeakSignalPath(bool on) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->lowSnr = on;
}
bool ArgosReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rateSet;
}
void ArgosReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetState();
    const uint64_t seq = p_->tel.seq;
    p_->tel = ArgosTelemetry();
    p_->tel.seq = seq + 1;
    p_->tel.inputRate = p_->fs;
    p_->tel.state = p_->rateSet ? 1 : 0;
    p_->tel.link = p_->link; p_->tel.linkSetting = p_->linkSet; p_->tel.dialHz = p_->dialHz;
}
void ArgosReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    ArgosTelemetry& t = p_->tel;
    t.log.clear(); t.ptts.clear(); t.frames.clear(); t.doppler.clear();
    t.dataValid = false;
    t.blocksOk = t.blocksBad = t.bursts = t.otherBursts = 0;
    t.dlBits = 0; t.dlFcsBad = 0;
    t.seq++;
}
void ArgosReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->log = std::move(cb);
}
bool ArgosReceiver::telemetry(ArgosTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->tel.seq <= lastSeq) return false;
    out = p_->tel;
    return true;
}

void ArgosReceiver::feed(const cf32* x, size_t n) {
    if (!x || !n) return;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& m = *p_;
    if (!m.rateSet) { m.tel.state = 0; return; }
    const double binW = m.fs / Impl::kDet;
    for (size_t i = 0; i < n; i++) {
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        m.accRe += re * m.loRe - im * m.loIm;
        m.accIm += re * m.loIm + im * m.loRe;
        m.accN++;
        const double nr = m.loRe * m.stRe - m.loIm * m.stIm, ni = m.loRe * m.stIm + m.loIm * m.stRe;
        m.loRe = nr; m.loIm = ni;
        if (++m.loCount >= 4096) { m.loCount = 0; const double a = 1.0 / std::sqrt(m.loRe * m.loRe + m.loIm * m.loIm); m.loRe *= a; m.loIm *= a; }
        m.nIn++;
        if ((double)m.nIn >= m.binEnd) {
            const double inv = 1.0 / (double)m.accN;
            m.sample128(m.accRe * inv, m.accIm * inv);
            m.accRe = m.accIm = 0; m.accN = 0;
            m.k128++;
            m.binEnd = (double)(m.k128 + 1) * binW;
        }
    }
    if (m.timeSec() - m.lastPublish >= 0.25) { m.lastPublish = m.timeSec(); m.publish(); }
}

}  // namespace dect2
