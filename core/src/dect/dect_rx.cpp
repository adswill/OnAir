// DECT monitor (see dect_rx.h).
#include "dect2/dect/dect_rx.h"
#include "dect2/dsp/gen_util.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kOut = 4.608e6;          // demodulator rate: 4 samples per bit
constexpr int kSps = 4;
constexpr int kSyncBits = 32;
constexpr int kMean = kSyncBits * kSps;
constexpr int kSearch = 80;                // alignments tried each side of the estimated end of the S-field, in samples
constexpr int kNeed = (64 + 2) * kSps + 8 + kSearch;   // samples after the sync a candidate waits for (the A-field)
constexpr int kHist = kSyncBits * kSps + 16 + kSearch;
}

struct DectReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rateSet = 0, offsetSet = 0;
    std::atomic<bool> resetReq{true}, clearReq{false};
    std::atomic<double> centerHz{0};

    double rate = 0, offset = 0;
    int64_t nIn = 0;
    std::complex<double> lo{1, 0}, loStep{1, 0};
    int loCount = 0;
    int L = 1;
    std::vector<float> taps, hr, hi;
    int hp = 0;
    double step = 1;
    int64_t outK = 0;
    std::complex<float> yF{0, 0}, prevY{0, 0};
    float box[kSps] = {}; int boxPos = 0; float boxSum = 0;
    std::vector<float> meanBuf; int meanPos = 0; double meanSum = 0;
    float pwBuf[kMean] = {}; int pwPos = 0; double pwSum = 0, noiseFloor = 1e30;
    std::vector<float> fb, pb;
    int64_t base = 0, g = -1, skipUntil = 0, skipUntilPkt = 0;
    struct Cand { int64_t at; bool fromBase; };
    std::deque<Cand> cands;
    DectTelemetry tel, pub;
    std::unordered_map<uint64_t, size_t> baseIdx;
    int lastBase = -1; double lastBaseSec = -1;
    double power = 0; int64_t nPower = 0;
    double nextReport = 0, lastGood = -100;
    double snrAvg = 0, thrAvg = 0;

    double sigSec() const { return rate > 0 ? nIn / rate : 0; }

    void initFront() {
        lo = 1; loCount = 0;
        loStep = rate > 0 ? std::polar(1.0, -2 * kPi * offset / rate) : std::complex<double>(1, 0);
        step = rate / kOut;
        L = std::max(15, (int)std::ceil(13.0 * rate / 1e6)) | 1;
        taps.assign((size_t)L, 0.f);
        const double fc = 650e3 / rate;   // the GFSK main lobe (about +-0.7 MHz) passes
        double sum = 0;
        for (int i = 0; i < L; i++) {
            const double m = i - (L - 1) / 2.0;
            const double s = m == 0 ? 2 * fc : std::sin(2 * kPi * fc * m) / (kPi * m);
            const double w = 0.54 - 0.46 * std::cos(2 * kPi * i / (L - 1));
            taps[(size_t)i] = (float)(s * w);
            sum += s * w;
        }
        for (float& t : taps) t = (float)(t / sum);
        std::reverse(taps.begin(), taps.end());
        hr.assign((size_t)L * 2, 0.f); hi.assign((size_t)L * 2, 0.f); hp = 0;
        outK = 0; nIn = 0;
        yF = prevY = std::complex<float>(0, 0);
    }
    void resetDemod() {
        for (float& b : box) b = 0;
        boxPos = 0; boxSum = 0;
        meanBuf.assign(kMean, 0.f); meanPos = 0; meanSum = 0;
        for (float& p : pwBuf) p = 0;
        pwPos = 0; pwSum = 0; noiseFloor = 1e30;
        fb.clear(); pb.clear(); base = 0; g = -1; skipUntil = 0; skipUntilPkt = 0; burstOn = false;
        cands.clear();
    }
    void resetState() {
        { std::lock_guard<std::mutex> lk(mu); rate = rateSet; offset = offsetSet; }
        initFront();
        resetDemod();
        power = 0; nPower = 0; nextReport = 0.25 * rate; lastGood = -100;
        tel.inputRate = rate;
        tel.timeSec = 0;
        for (auto& b : tel.bases) { b.firstSec = b.lastSec = 0; }
    }
    void clearAll() {
        const uint64_t seq = tel.seq;
        tel = DectTelemetry(); tel.seq = seq; tel.inputRate = rate;
        baseIdx.clear(); phase.clear(); lastBase = -1; snrAvg = thrAvg = 0;
    }

    void input(float re, float im) {
        // the two complex products written out (the same bits as std::complex<double>, without its NaN-recovery call)
        const double lr = lo.real(), li = lo.imag(), sr = loStep.real(), si = loStep.imag();
        const std::complex<double> v((double)re * lr - (double)im * li, (double)re * li + (double)im * lr);
        lo = std::complex<double>(lr * sr - li * si, lr * si + li * sr);
        if (++loCount >= 4096) { lo /= std::abs(lo); loCount = 0; }
        hr[(size_t)hp] = hr[(size_t)(hp + L)] = (float)v.real();
        hi[(size_t)hp] = hi[(size_t)(hp + L)] = (float)v.imag();
        hp = hp + 1 == L ? 0 : hp + 1;
        const int64_t i = nIn;
        double t = outK * step;
        int64_t f = (int64_t)std::floor(t);
        if (i != f && i != f + 1) return;
        float o[2];
        genutil::dot2(taps.data(), hr.data() + hp, hi.data() + hp, L, o);
        const std::complex<float> y(o[0], o[1]);
        for (;;) {
            t = outK * step; f = (int64_t)std::floor(t);
            const float fr = (float)(t - (double)f);
            if (i == f) {
                yF = y;
                if (fr < 1e-6f) { outK++; demod(y); continue; }
                break;
            }
            if (i == f + 1) { outK++; demod(yF * (1 - fr) + y * fr); continue; }
            break;
        }
    }

    bool burstOn = false;
    std::map<uint64_t, double> phase;   // frame phase of each base station's bursts, in seconds

    float at(int64_t k) const { return fb[(size_t)(k - base)]; }

    __attribute__((always_inline)) void demod(std::complex<float> y) {   // inlined: a call per sample stored the two floats and loaded them as one 8-byte value (store-forwarding stall)
        const std::complex<float> z = y * std::conj(prevY);
        prevY = y;
        const float fHz = (float)(std::atan2(z.imag(), z.real()) * (kOut / (2 * kPi)));
        boxSum += fHz - box[boxPos]; box[boxPos] = fHz; boxPos = (boxPos + 1) % kSps;
        const float s = boxSum / kSps;
        meanSum += s - meanBuf[(size_t)meanPos]; meanBuf[(size_t)meanPos] = s; meanPos = (meanPos + 1) % kMean;
        const float pw = std::norm(y);
        pwSum += pw - pwBuf[pwPos]; pwBuf[pwPos] = pw; pwPos = (pwPos + 1) % kMean;
        if (pwPos == 0) { double e = 0; for (float v : pwBuf) e += v; pwSum = e; }   // the running sum drifts: after a burst into digital silence it went below
                                                                                     // zero, the floor with it, and every sample then looked like a burst start
        const double pAvg = std::max(pwSum / kMean, 1e-12);
        if (g < kMean) {}
        else if (pAvg < noiseFloor) noiseFloor = pAvg;
        else noiseFloor += (pAvg - noiseFloor) * 2e-7;
        fb.push_back(s);
        pb.push_back(pw);
        g++;
        // a burst: the power over 32 bits rises well above the floor. The S-field ends about 22 bits after that point (it depends on
        // the signal level), so the decoder tries every alignment within +-20 bits of it, with both sync patterns.
        if (g >= kMean) {
            if (!burstOn && pAvg > 6.0 * noiseFloor) { burstOn = true; cands.push_back({g + 22 * kSps, true}); }
            else if (burstOn && pAvg < 3.0 * noiseFloor) burstOn = false;
        }
    }

    void process() {
        const int64_t end = base + (int64_t)fb.size();
        while (!cands.empty() && cands.front().at + kNeed < end) {
            const Cand c = cands.front();
            cands.pop_front();
            if (c.at < skipUntilPkt) continue;
            decode(c);
        }
        const int64_t keepFrom = (cands.empty() ? end : cands.front().at) - kHist;
        if (keepFrom - base > 65536) {
            const size_t drop = (size_t)(keepFrom - base);
            fb.erase(fb.begin(), fb.begin() + (ptrdiff_t)drop);
            pb.erase(pb.begin(), pb.begin() + (ptrdiff_t)drop);
            base += (int64_t)drop;
        }
    }

    void decode(const Cand& cd) {
        if (!decodeDir(cd, true)) decodeDir(cd, false);
    }

    bool decodeDir(const Cand& cd0, bool fromBase) {
        Cand cd = cd0; cd.fromBase = fromBase;
        const uint32_t known = cd.fromBase ? dect::kSyncRfp : dect::kSyncPp;
        // sample phases ordered by the eye over the known 32 bits; the first whose A-field CRC passes wins
        struct Ph { int64_t c; double eye, m1, m0; int errs; };
        std::vector<Ph> ph;
        for (int d = -kSearch; d <= kSearch; d++) {   // the sync correlation is flat near its peak (Gaussian filtering): try the bit offsets around it too
            const int64_t c = cd.at + d;
            if (c - (kSyncBits - 1) * kSps < base || c + kNeed - 8 > base + (int64_t)fb.size()) continue;
            double m1 = 0, m0 = 0; int n1 = 0, n0 = 0;
            for (int k = 0; k < kSyncBits; k++) {
                const float v = at(c - (kSyncBits - 1 - k) * kSps);
                if (known >> (kSyncBits - 1 - k) & 1) { m1 += v; n1++; } else { m0 += v; n0++; }
            }
            if (!n1 || !n0) continue;
            m1 /= n1; m0 /= n0;
            int errs = 0;
            for (int k = 0; k < kSyncBits; k++) errs += ((at(c - (kSyncBits - 1 - k) * kSps) > (m1 + m0) / 2) ? 1 : 0) != (int)(known >> (kSyncBits - 1 - k) & 1);
            ph.push_back({c, m1 - m0, m1, m0, errs});
        }
        std::sort(ph.begin(), ph.end(), [](const Ph& a, const Ph& b) { return a.eye > b.eye; });
        if (ph.empty()) return false;
        int bestErrs = 99; for (const Ph& p : ph) bestErrs = std::min(bestErrs, p.errs);
        for (Ph& p : ph) {
            const double thr = (p.m1 + p.m0) / 2;
            uint64_t raw = 0;
            for (int j = 0; j < 64; j++) raw = raw << 1 | (at(p.c + (int64_t)(j + 1) * kSps) > thr ? 1u : 0u);
            const dect::AField a = dect::parseAField(raw);
            if (!a.crcOk) continue;
            if (p.errs > 3) continue;   // the S-field at this alignment must be the pattern too (a CRC alone passes by chance 1 in 30000)
            // levels and noise from the 96 known bits (S-field and A-field: their values are now certain)
            double var = 0, h1 = 0, h0 = 0; int c1 = 0, c0 = 0;
            for (int pass = 0; pass < 2; pass++) {
                if (pass) { h1 /= c1; h0 /= c0; }
                for (int k = 0; k < kSyncBits + 64; k++) {
                    const int bit = k < kSyncBits ? (int)(known >> (kSyncBits - 1 - k) & 1) : (int)(raw >> (63 - (k - kSyncBits)) & 1);
                    const double v = at(p.c - (kSyncBits - 1 - k) * kSps);
                    if (!pass) { if (bit) { h1 += v; c1++; } else { h0 += v; c0++; } }
                    else { const double e = v - (bit ? h1 : h0); var += e * e; }
                }
            }
            var /= (kSyncBits + 64);
            p.m1 = h1; p.m0 = h0;
            const int64_t pEnd = p.c + 65 * kSps;
            skipUntilPkt = pEnd; skipUntil = std::max(skipUntil, pEnd);
            double pw = 0; int np = 0;
            for (int64_t k = std::max(base, p.c - kSyncBits * kSps); k < pEnd && k < base + (int64_t)pb.size(); k++) { pw += pb[(size_t)(k - base)]; np++; }
            const float rssi = (float)(10 * std::log10(pw / std::max(1, np) + 1e-20));
            const double dev = (p.m1 - p.m0) / 2, snr = var > 0 ? 10 * std::log10(dev * dev / var) : 40;
            snrAvg = tel.blocksOk == 0 ? snr : 0.8 * snrAvg + 0.2 * snr;
            thrAvg = tel.blocksOk == 0 ? thr : 0.9 * thrAvg + 0.1 * thr;
            tel.cfoHz = thrAvg;
            accept(a, cd.fromBase, rssi, (double)(p.c - kSyncBits * kSps) / kOut);
            return true;
        }
        // no phase gave a good CRC: a sync-like pattern in noise, or a damaged burst
        if (bestErrs <= 1) { if (cd.fromBase) tel.baseBursts++; else tel.handsetBursts++; tel.blocksBad++; return true; }
        return false;
    }

    void accept(const dect::AField& a, bool fromBase, float rssi, double sec) {
        tel.blocksOk++;
        if (fromBase) tel.baseBursts++; else tel.handsetBursts++;
        if (a.crcInverted) tel.crcInverted++; else tel.crcPlain++;
        lastGood = sigSec();
        const int64_t wall = (int64_t)time(nullptr);
        DectLog l; l.wall = wall; l.sec = sec; l.fromBase = fromBase; l.crcOk = true; l.ta = a.ta; l.tail = a.tail; l.rssi = rssi;
        tel.log.insert(tel.log.begin(), l);
        if (tel.log.size() > kDectLogMax) tel.log.pop_back();
        if (!fromBase) return;
        int bi = -1;
        if (a.ta == dect::TaNt || a.ta == dect::TaNtCl) {
            const dect::Rfpi r = dect::rfpiOf(a.tail);
            auto it = baseIdx.find(r.value);
            if (it == baseIdx.end()) {
                if (tel.bases.size() >= kDectBaseMax) tel.bases.pop_back();
                DectBase b; b.rfpi = r.value; b.arc = r.arc; b.emc = r.emc; b.fpn = r.fpn;
                b.firstSec = sec; b.firstWall = wall;
                tel.bases.push_back(b);
                baseIdx.clear();
                for (size_t i = 0; i < tel.bases.size(); i++) baseIdx[tel.bases[i].rfpi] = i;
                bi = (int)tel.bases.size() - 1;
                if (log) { char s[120]; snprintf(s, sizeof s, "DECT: new base station %s%s", dect::hex40(r.value).c_str(), r.emc >= 0 ? (" (manufacturer code " + std::to_string(r.emc) + ")").c_str() : ""); log(s); }
            } else bi = (int)it->second;
            tel.bases[(size_t)bi].ntSeen++;
            phase[r.value] = std::fmod(sec, dect::kFrameSec);
        } else {
            // the other tails belong to the base station that sends in this slot of the frame (the frame clock is the same for its Nt bursts)
            const double ph = std::fmod(sec, dect::kFrameSec);
            for (size_t i = 0; i < tel.bases.size(); i++) {
                auto it = phase.find(tel.bases[i].rfpi);
                if (it == phase.end()) continue;
                double d = std::fabs(ph - it->second); d = std::min(d, dect::kFrameSec - d);
                if (d < 150e-6) { bi = (int)i; break; }
            }
            if (bi >= 0 && a.ta == dect::TaQt) { tel.bases[(size_t)bi].qtSeen++; tel.bases[(size_t)bi].qtLast = a.tail; }
        }
        if (bi < 0) return;
        lastBase = bi; lastBaseSec = sec;
        DectBase& b = tel.bases[(size_t)bi];
        b.frames++; b.rssi = rssi; b.lastSec = sec; b.lastWall = wall;
        const double rel = std::fmod(std::max(0.0, sec - b.firstSec) + dect::kFrameSec * 0.5 / dect::kSlots, dect::kFrameSec) / (dect::kFrameSec / dect::kSlots);
        b.slotsMask |= 1u << ((int)rel % dect::kSlots);
    }

    void report() {
        tel.seq++;
        tel.timeSec = sigSec();
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        const double c = centerHz.load();
        tel.channelMhz = c > 0 ? (c + offset) / 1e6 : 0;
        int cn = -1;
        tel.band = c > 0 ? dect::bandOf(tel.channelMhz, &cn) : -1;
        tel.carrier = tel.band >= 0 ? cn : -1;
        tel.snrDb = (float)snrAvg;
        tel.state = sigSec() - lastGood < 5 ? 1 : 0;
        tel.dataValid = tel.state >= 1;
        DectTelemetry out = tel;
        std::stable_sort(out.bases.begin(), out.bases.end(), [](const DectBase& x, const DectBase& y) { return x.lastSec > y.lastSec; });
        std::lock_guard<std::mutex> lk(mu);
        pub = std::move(out);
    }
};

DectReceiver::DectReceiver() : p_(std::make_unique<Impl>()) {}
DectReceiver::~DectReceiver() = default;
void DectReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void DectReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetSet) { p_->offsetSet = hz; p_->resetReq = true; } }
void DectReceiver::setCenterHz(double hz) { p_->centerHz = hz; }
bool DectReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= dectTuning().minSampleRate - 1; }
void DectReceiver::reset() { p_->resetReq = true; }
void DectReceiver::clearResults() {
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->pub.bases.clear(); p_->pub.log.clear();
        p_->pub.seq++;
    }
    p_->clearReq = true;
}

void DectReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    if (m.clearReq.exchange(false)) m.clearAll();
    if (m.rate < dectTuning().minSampleRate - 1 || n == 0) {
        if (m.rate > 0) { m.nIn += (int64_t)n; if ((double)m.nIn >= m.nextReport) { m.nextReport += 0.25 * m.rate; m.report(); } }
        return;
    }
    for (size_t i = 0; i < n; i++) {
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        m.power += (double)re * re + (double)im * im;
        m.input(re, im);
        m.nIn++;
        if ((double)m.nIn >= m.nextReport) { m.process(); m.nextReport += 0.25 * m.rate; m.report(); }
    }
    m.nPower += (int64_t)n;
    m.process();
}

bool DectReceiver::telemetry(DectTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void DectReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

ModeTuning dectTuning() {
    ModeTuning t;
    t.stdMode = 80; t.id = "dect"; t.name = "DECT cordless phones";
    t.minMhz = 900; t.maxMhz = 1930; t.defMhz = dect::carrierMhz(0);
    t.sampleRate = 5000000;
    t.basebandHz = 0;
    t.bandwidthMhz = 1.728;
    t.minSampleRate = 4608000;
    t.tuneOffsetHz = 1000000;
    return t;
}

}  // namespace dect2
