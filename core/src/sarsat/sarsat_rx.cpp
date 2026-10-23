// COSPAS-SARSAT 406 MHz receiver (see sarsat_rx.h).
//
// Burst finder: the stream is mixed to the dial offset, averaged down to 32 kHz and cut into blocks of 1024 samples (32 ms); a Hann-windowed FFT of each block
// is searched for one line that stands out from the median bin (the 160 ms unmodulated preamble) and that was not already there a moment ago (radio DC spikes
// and carriers do not count). A burst runs from the first block with a line until 6 blocks without one. Its carrier frequency is the median of the first lines.
// Demodulator: the burst is rotated to zero frequency; each bit is two half-bit vector sums, and the angle between them (Manchester phase modulation of
// +-1.1 rad turns the carrier phase by +2.2 rad between the halves of a bit, whatever the carrier phase is) gives the bit. Bit timing, bit rate (+-1 %) and
// polarity are found by sliding the 24 sync bits over the burst; the best candidates are decoded and the one the BCH check repairs with the fewest changes wins.
#include "dect2/sarsat/sarsat_rx.h"
#include "dect2/sarsat/sarsat_tel.h"
#include "dect2/sarsat/sgb_rx.h"
#include "dect2/sarsat/sarsat_gen.h"
#include "dect2/common/mathconst.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <ctime>
#include <mutex>

namespace dect2 {

using dect2::math::kPi;
using sarsat::kFs;
using sarsat::kBitRate;

ModeTuning sarsatTuning() {
    ModeTuning t;
    t.stdMode = 94;
    t.id = "sarsat";
    t.name = "COSPAS-SARSAT 406 MHz";
    t.minMhz = 406.0;
    t.maxMhz = 406.1;
    t.defMhz = sarsat::kDefaultMhz;
    t.sampleRate = 256000;
    t.basebandHz = 16000;
    t.bandwidthMhz = 0.1;
    t.minSampleRate = 128000;
    t.tuneOffsetHz = 20000;
    return t;
}

namespace sarsat {

namespace {

using cd = std::complex<double>;

struct Cand {
    int matches = 0;
    double conf = 0;
    double t0 = 0, r = 0;
    bool inv = false, test = false;
};

struct Slicer {
    std::vector<cd> P;   // prefix sums of the rotated burst
    size_t n = 0;
    bool soft(double b0, double spb, double& d) const {
        const double bm = b0 + spb * 0.5, b1 = b0 + spb;
        if (b0 < 0 || b1 > (double)n) return false;
        const size_t i0 = (size_t)std::lround(b0), im = (size_t)std::lround(bm), i1 = std::min(n, (size_t)std::lround(b1));
        const cd h1 = P[im] - P[i0], h2 = P[i1] - P[im];
        const double m = std::abs(h1) * std::abs(h2);
        if (m <= 1e-12) { d = 0; return true; }
        d = (h1 * std::conj(h2)).imag() / m;
        return true;
    }
    // the first `count` bits starting at t0 with bit period spb; stops at the end of the burst
    std::vector<double> bits(double t0, double spb, int count) const {
        std::vector<double> v;
        for (int k = 0; k < count; k++) {
            double d;
            if (!soft(t0 + k * spb, spb, d)) break;
            v.push_back(d);
        }
        return v;
    }
};

Cand syncScore(const Slicer& s, double t0, double r) {
    Cand c;
    c.t0 = t0; c.r = r;
    const double spb = kFs / kBitRate * (1 + r);
    const std::vector<double> d = s.bits(t0, spb, kSyncBits);
    if (d.size() < (size_t)kSyncBits) return c;
    for (int test = 0; test < 2; test++) {
        const std::vector<uint8_t> ref = syncBits(test != 0);
        for (int inv = 0; inv < 2; inv++) {
            int m = 0;
            double conf = 0;
            for (int k = 0; k < kSyncBits; k++) {
                const int want = (ref[(size_t)k] ^ inv) ? 1 : -1;
                const int got = d[(size_t)k] > 0 ? 1 : -1;
                if (want == got) m++;
                conf += want * d[(size_t)k];
            }
            if (m > c.matches || (m == c.matches && conf > c.conf)) { c.matches = m; c.conf = conf; c.inv = inv != 0; c.test = test != 0; }
        }
    }
    return c;
}

bool tryDecode(const Slicer& s, const Cand& c, Beacon& out, int& matches) {
    const double spb = kFs / kBitRate * (1 + c.r);
    const std::vector<double> d = s.bits(c.t0, spb, kLongBits);
    if (d.size() < (size_t)kShortBits) return false;
    std::vector<uint8_t> b(d.size());
    for (size_t k = 0; k < d.size(); k++) b[k] = (uint8_t)((d[k] > 0) != c.inv);
    // a real burst starts with 160 ms of bare carrier: the two halves of the 20 bit periods before the sync point the same way (cosine of the angle near 1);
    // data and noise do not do that. This keeps a burst that was caught in the middle, or noise, from passing as a message.
    {
        double cs = 0;
        int cnt = 0;
        for (int k = 1; k <= 20; k++) {
            const double b0 = c.t0 - k * spb;
            if (b0 < 0) break;
            const size_t i0 = (size_t)std::lround(b0), im = (size_t)std::lround(b0 + spb * 0.5), i1 = (size_t)std::lround(b0 + spb);
            const cd h1 = s.P[im] - s.P[i0], h2 = s.P[i1] - s.P[im];
            const double m = std::abs(h1) * std::abs(h2);
            if (m > 1e-12) { cs += (h1 * std::conj(h2)).real() / m; cnt++; }
        }
        if (cnt < 16 || cs / cnt < 0.35) return false;
    }
    // the sync bits must be the ones found
    const std::vector<uint8_t> ref = syncBits(c.test);
    matches = 0;
    for (int k = 0; k < kSyncBits; k++) matches += b[(size_t)k] == ref[(size_t)k];
    if (matches < 23) return false;
    Beacon o;
    if (!decode(b, o)) return false;
    o.selfTest = c.test;
    o.rawHex = bitsToHex(b, 1, o.longMsg ? kLongBits : kShortBits);
    out = o;
    return true;
}

}  // namespace

bool demodBurst(const std::vector<cf32>& x, double carrierHz, Beacon& out) {
    const size_t n = x.size();
    if (n < (size_t)(kFs * 0.4)) return false;
    Slicer s;
    s.n = n;
    s.P.assign(n + 1, cd(0, 0));
    cd lo(1, 0);
    const double w = -2 * kPi * carrierHz / kFs;
    const cd st(std::cos(w), std::sin(w));
    for (size_t i = 0; i < n; i++) {
        s.P[i + 1] = s.P[i] + cd(x[i].real(), x[i].imag()) * lo;
        lo *= st;
        if ((i & 1023) == 1023) lo /= std::abs(lo);
    }
    const double spb0 = kFs / kBitRate;
    const double rates[] = {-0.01, -0.0075, -0.005, -0.0025, 0.0, 0.0025, 0.005, 0.0075, 0.01};
    std::vector<Cand> cands;
    for (double r : rates) {
        const double spb = spb0 * (1 + r);
        for (double t0 = 0; t0 + kSyncBits * spb < (double)n; t0 += 4) {
            const Cand c = syncScore(s, t0, r);
            if (c.matches >= 22) cands.push_back(c);
        }
    }
    if (cands.empty()) return false;
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.matches != b.matches ? a.matches > b.matches : a.conf > b.conf; });
    if (cands.size() > 10) cands.resize(10);
    bool found = false;
    int bestCorr = 99;
    double bestConf = -1;
    for (const Cand& c0 : cands) {
        for (double dt = -4; dt <= 4; dt += 1) {
            for (double dr = -0.0025; dr <= 0.0026; dr += 0.0025) {
                Cand c = syncScore(s, c0.t0 + dt, c0.r + dr);
                if (c.matches < 23) continue;
                Beacon o;
                int m = 0;
                if (!tryDecode(s, c, o, m)) continue;
                if (o.corrected < bestCorr || (o.corrected == bestCorr && c.conf > bestConf)) { bestCorr = o.corrected; bestConf = c.conf; out = o; found = true; }
            }
        }
    }
    return found;
}

}  // namespace sarsat

struct SarsatReceiver::Impl {
    std::mutex mu;
    SarsatTelemetry tel;
    std::function<void(const std::string&)> log;
    double fs = 0, offHz = 0;
    bool rateSet = false;
    // whole-capture search: child receivers 90 kHz apart
    bool wide = false, kidsDirty = true;
    std::vector<std::unique_ptr<SarsatReceiver>> kids;
    std::vector<double> kidShift;              // each child's centre from the dial
    std::vector<uint64_t> kidSeq;
    std::vector<SarsatEntry> pending;          // decoded in some channel, waiting 1.5 s for a better copy of the same burst
    double wideSpan = 0;

    // mixer and the average down to 32 kHz
    double loRe = 1, loIm = 0, stRe = 1, stIm = 0;
    int loCount = 0;
    double accRe = 0, accIm = 0;
    int accN = 0;
    uint64_t nIn = 0, k32 = 0;
    double binEnd = 0;

    // second-generation beacons (C/S T.018): a second mixer (406.050 MHz to zero) and an average down to 153.6 kHz, needs an input rate of 160 kHz or more
    sarsat::SgbDetector sgb;
    double sRe = 1, sIm = 0, sStRe = 1, sStIm = 0, sAccRe = 0, sAccIm = 0, sBinEnd = 0;
    int sCount = 0, sAccN = 0;
    uint64_t sIn = 0, sK = 0, sOut = 0;
    bool sgbOn = false;
    std::vector<cf32> sBuf;

    // the 32 kHz stream
    static constexpr double kDet = 128000.0;   // detection rate: +-46 kHz searched, the 100 kHz transponder passband
    static constexpr size_t kRing = 262144, kBlock = 4096;
    std::vector<cf32> ring = std::vector<cf32>(kRing);
    uint64_t n32 = 0;
    std::vector<float> win = std::vector<float>(kBlock);
    Fft fft{(int)kBlock};
    std::vector<float> bg = std::vector<float>(kBlock, 0.f);
    int warm = 0;

    // burst state
    bool active = false;
    int miss = 0, dets = 0;
    uint64_t firstBlockStart = 0;
    std::vector<double> peaks;
    float peakSnr = -200, peakLevel = -200;
    double lastDecodeSec = -1e9, lastPublish = 0;

    Impl() {
        for (size_t i = 0; i < kBlock; i++) win[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)kBlock));
    }

    void resetState() {
        loRe = 1; loIm = 0;
        const double w = -2 * kPi * offHz / (fs > 0 ? fs : 1);
        stRe = std::cos(w); stIm = std::sin(w);
        accRe = accIm = 0; accN = 0; nIn = 0; k32 = 0;
        binEnd = fs > 0 ? fs / kDet : 0;
        sgbOn = fs >= 160000.0;
        sRe = 1; sIm = 0; sAccRe = sAccIm = 0; sAccN = 0; sIn = 0; sK = 0; sOut = 0; sCount = 0;
        sBinEnd = fs > 0 ? fs / sarsat::kSgbFs : 0;
        { const double w2 = -2 * kPi * (offHz + sarsat::kSgbOffsetHz) / (fs > 0 ? fs : 1); sStRe = std::cos(w2); sStIm = std::sin(w2); }
        sgb.reset(); sBuf.clear();
        n32 = 0;
        std::fill(bg.begin(), bg.end(), 0.f);
        warm = 0; active = false; miss = 0; dets = 0; peaks.clear();
        lastDecodeSec = -1e9; lastPublish = 0;
    }

    double timeSec() const { return (double)n32 / kDet; }

    void publish() {
        tel.timeSec = timeSec();
        tel.state = rateSet ? (timeSec() - lastDecodeSec < 300 ? 2 : 1) : 0;
        tel.seq++;
    }

    void finishBurst() {
        active = false;
        if (dets >= 4 && peakSnr >= 10.f) {
            tel.bursts++;
            const uint64_t start = firstBlockStart >= 8192 ? firstBlockStart - 8192 : 0;
            const uint64_t from = n32 > kRing - 4096 && start < n32 - (kRing - 4096) ? n32 - (kRing - 4096) : start;
            std::vector<cf32> x;
            x.reserve((size_t)(n32 - from));
            for (uint64_t i = from; i < n32; i++) x.push_back(ring[(size_t)(i & (kRing - 1))]);
            std::vector<double> pk(peaks.begin(), peaks.begin() + std::min<size_t>(4, peaks.size()));
            std::sort(pk.begin(), pk.end());
            const double carrier = pk[pk.size() / 2];
            // mix the carrier to 0 and average 4:1 down to the 32 kHz the demodulator works at
            std::vector<cf32> y;
            y.reserve(x.size() / 4 + 1);
            {
                const double w = -2 * kPi * carrier / kDet;
                double ph = 0;
                cf32 acc(0, 0);
                for (size_t i = 0; i < x.size(); i++) {
                    acc += x[i] * cf32((float)std::cos(ph), (float)std::sin(ph));
                    ph += w;
                    if ((i & 3) == 3) { y.push_back(acc * 0.25f); acc = cf32(0, 0); }
                }
            }
            sarsat::Beacon b;
            const bool got = sarsat::demodBurst(y, 0.0, b);
            if (got) {
                SarsatEntry e;
                e.wallTime = (int64_t)std::time(nullptr);
                e.timeSec = timeSec();
                e.b = b;
                e.snrDb = peakSnr; e.levelDb = peakLevel; e.cfoHz = (float)carrier;
                tel.log.insert(tel.log.begin(), e);
                if (tel.log.size() > kSarsatLogMax) tel.log.pop_back();
                tel.blocksOk++;
                tel.dataValid = true;
                lastDecodeSec = timeSec();
                if (log) {
                    char line[256];
                    snprintf(line, sizeof line, "COSPAS-SARSAT %s %s %s%s", b.hexId.c_str(), b.beaconType.c_str(), b.country.c_str(), b.hasPos ? " (position)" : "");
                    log(line);
                }
            } else {
                tel.blocksBad++;
            }
            tel.cfoHz = carrier; tel.snrDb = peakSnr; tel.levelDb = peakLevel;
        }
        peaks.clear(); dets = 0; peakSnr = -200; peakLevel = -200;
        publish();
    }

    void block() {   // the last kBlock samples of the ring
        const uint64_t base = n32 - kBlock;
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
        const size_t kMax = (size_t)(46000.0 / kDet * (double)kBlock);   // keep to +-46 kHz: the edges are attenuated by the average
        auto allowed = [&](size_t k) { return k <= kMax || k >= kBlock - kMax; };
        std::vector<float> sel;
        sel.reserve(kBlock);
        for (size_t k = 0; k < kBlock; k++) if (allowed(k)) sel.push_back(p[k]);
        std::nth_element(sel.begin(), sel.begin() + (long)(sel.size() / 2), sel.end());
        const float med = std::max(sel[sel.size() / 2], 1e-20f);
        size_t pk = 0;
        float pmax = 0;
        for (size_t k = 0; k < kBlock; k++) if (allowed(k) && p[k] > pmax && p[k] > 6.f * bg[k]) { pmax = p[k]; pk = k; }
        bool det = warm >= 12 && pmax >= 30.f * med;
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
        if (det) {
            if (!active) { active = true; firstBlockStart = base; miss = 0; dets = 0; peakSnr = -200; peakLevel = -200; peaks.clear(); }
            miss = 0; dets++;
            peaks.push_back(f);
            peakSnr = std::max(peakSnr, snr);
            peakLevel = std::max(peakLevel, 10.f * std::log10((float)(pw / kBlock) + 1e-12f));
        } else if (active) {
            miss++;
        }
        if (active && (miss >= 6 || n32 - firstBlockStart > (uint64_t)(0.9 * kDet))) finishBurst();
        if (!active && !det) {
            for (size_t k = 0; k < kBlock; k++) bg[k] = warm == 0 ? std::min(p[k], 4.f * med) : bg[k] * 0.98f + std::min(p[k], 4.f * med) * 0.02f;   // a carrier present during the warm-up must not be learnt as background
            if (warm < 1000) warm++;
        }
        tel.levelDb = active ? tel.levelDb : 10.f * std::log10((float)(pw / kBlock) + 1e-12f);
    }

    void sample32(double re, double im) {
        ring[(size_t)(n32 & (kRing - 1))] = cf32((float)re, (float)im);
        n32++;
        if (n32 % kBlock == 0) block();
    }
};

SarsatReceiver::SarsatReceiver() : p_(std::make_unique<Impl>()) {}
SarsatReceiver::~SarsatReceiver() = default;

void SarsatReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->fs = inputRateHz;
    p_->kidsDirty = true;
    p_->rateSet = inputRateHz >= sarsatTuning().minSampleRate - 1;
    p_->resetState();
    p_->tel.inputRate = inputRateHz;
    p_->tel.state = p_->rateSet ? 1 : 0;
    p_->tel.seq++;
}
void SarsatReceiver::setSignalOffset(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (hz == p_->offHz) return;
    p_->offHz = hz;
    p_->kidsDirty = true;
    const double w = -2 * kPi * hz / (p_->fs > 0 ? p_->fs : 1);
    p_->stRe = std::cos(w); p_->stIm = std::sin(w);
    const double w2 = -2 * kPi * (hz + sarsat::kSgbOffsetHz) / (p_->fs > 0 ? p_->fs : 1);
    p_->sStRe = std::cos(w2); p_->sStIm = std::sin(w2);
}
bool SarsatReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rateSet;
}
void SarsatReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetState();
    p_->kidsDirty = true;
    const uint64_t seq = p_->tel.seq;
    p_->tel = SarsatTelemetry();
    p_->tel.seq = seq + 1;
    p_->tel.inputRate = p_->fs;
    p_->tel.state = p_->rateSet ? 1 : 0;
}
void SarsatReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->tel.log.clear();
    p_->tel.dataValid = false;
    p_->tel.blocksOk = p_->tel.blocksBad = p_->tel.bursts = 0;
    p_->tel.seq++;
}
void SarsatReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->log = std::move(cb);
}
bool SarsatReceiver::telemetry(SarsatTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->tel.seq <= lastSeq) return false;
    out = p_->tel;
    return true;
}

void SarsatReceiver::setWideSearch(bool on) { std::lock_guard<std::mutex> lk(p_->mu); if (on != p_->wide) { p_->wide = on; p_->kidsDirty = true; } }
bool SarsatReceiver::wideSearch() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->wide; }
double SarsatReceiver::wideSpanHz() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->wide ? p_->wideSpan : 0; }

void SarsatReceiver::feed(const cf32* x, size_t n) {
    if (!x || !n) return;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& m = *p_;
    if (!m.rateSet) { m.tel.state = 0; return; }
    if (m.wide) {
        if (m.kidsDirty) {
            m.kidsDirty = false;
            m.kids.clear(); m.kidShift.clear(); m.kidSeq.clear(); m.pending.clear();
            const double span = std::min(0.85 * m.fs, 1.1e6);
            const int k = std::max(1, (int)std::ceil(span / 90e3));
            m.wideSpan = k * 90e3;
            for (int i = 0; i < k; i++) {
                const double d = (i - (k - 1) / 2.0) * 90e3;
                auto r = std::make_unique<SarsatReceiver>();
                r->configure(m.fs);
                r->setSignalOffset(m.offHz + d);
                m.kids.push_back(std::move(r)); m.kidShift.push_back(d); m.kidSeq.push_back(0);
            }
            m.tel.wideSpanHz = m.wideSpan; m.tel.wideChannels = k;
        }
        bool changed = false;
        for (size_t i = 0; i < m.kids.size(); i++) {
            m.kids[i]->feed(x, n);
            SarsatTelemetry kt;
            const uint64_t oldBurst = m.kidSeq[i];
            if (!m.kids[i]->telemetry(kt, 0)) continue;
            m.tel.levelDb = kt.levelDb; m.tel.timeSec = kt.timeSec;
            // new entries of this child: those past the count it had (log is newest first)
            const uint64_t total = kt.blocksOk;
            for (uint64_t j = 0; j < total - std::min(total, oldBurst) && j < kt.log.size(); j++) {
                SarsatEntry e = kt.log[(size_t)j];
                e.cfoHz += (float)m.kidShift[i];
                // one burst is heard in several channels: next to it (the overlap) and as weaker aliases further away, which can even pass
                // the BCH check with corrections. Keep the strongest copy (then the one with fewer corrections) of each burst time.
                bool merged = false;
                for (auto& o : m.pending)
                    if (std::fabs(o.timeSec - e.timeSec) < 0.4) {
                        if (e.snrDb > o.snrDb + 0.5f || (std::fabs(e.snrDb - o.snrDb) <= 0.5f && e.b.corrected < o.b.corrected)) o = e;
                        merged = true; break;
                    }
                if (!merged) m.pending.push_back(e);
            }
            m.kidSeq[i] = total;
            m.tel.blocksBad = std::max(m.tel.blocksBad, kt.blocksBad);
        }
        m.n32 += (uint64_t)((double)n / std::max(1.0, m.fs) * Impl::kDet);
        for (size_t q = 0; q < m.pending.size();) {
            if (m.timeSec() - m.pending[q].timeSec < 1.5) { q++; continue; }
            const SarsatEntry e = m.pending[q];
            m.pending.erase(m.pending.begin() + (long)q);
            m.tel.log.insert(m.tel.log.begin(), e);
            if (m.tel.log.size() > kSarsatLogMax) m.tel.log.pop_back();
            m.tel.blocksOk++; m.tel.bursts++; m.tel.dataValid = true; m.tel.cfoHz = e.cfoHz; m.tel.snrDb = e.snrDb;
            m.lastDecodeSec = e.timeSec;
            changed = true;
            if (m.log) {
                char line[256];
                snprintf(line, sizeof line, "COSPAS-SARSAT %s %s %s at %+.1f kHz", e.b.hexId.c_str(), e.b.beaconType.c_str(), e.b.country.c_str(), e.cfoHz / 1e3);
                m.log(line);
            }
        }
        if (changed || m.timeSec() - m.lastPublish >= 0.25) { m.lastPublish = m.timeSec(); m.publish(); }
        return;
    }
    if (!m.kids.empty()) { m.kids.clear(); m.tel.wideSpanHz = 0; m.tel.wideChannels = 0; }
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
            m.sample32(m.accRe * inv, m.accIm * inv);
            m.accRe = m.accIm = 0; m.accN = 0;
            m.k32++;
            m.binEnd = (double)(m.k32 + 1) * binW;
        }
        if (m.sgbOn) {
            m.sAccRe += re * m.sRe - im * m.sIm;
            m.sAccIm += re * m.sIm + im * m.sRe;
            m.sAccN++;
            const double qr = m.sRe * m.sStRe - m.sIm * m.sStIm, qi = m.sRe * m.sStIm + m.sIm * m.sStRe;
            m.sRe = qr; m.sIm = qi;
            if (++m.sCount >= 4096) { m.sCount = 0; const double a = 1.0 / std::sqrt(m.sRe * m.sRe + m.sIm * m.sIm); m.sRe *= a; m.sIm *= a; }
            m.sIn++;
            if ((double)m.sIn >= m.sBinEnd) {
                const double inv = 1.0 / (double)m.sAccN;
                m.sBuf.push_back(cf32((float)(m.sAccRe * inv), (float)(m.sAccIm * inv)));
                m.sAccRe = m.sAccIm = 0; m.sAccN = 0;
                m.sK++;
                m.sBinEnd = (double)(m.sK + 1) * (m.fs / sarsat::kSgbFs);
            }
        }
    }
    if (m.sgbOn && !m.sBuf.empty()) {
        m.sOut += m.sBuf.size();
        m.sgb.push(m.sBuf.data(), m.sBuf.size());
        m.sBuf.clear();
        for (const sarsat::SgbHit& h : m.sgb.take()) {
            SarsatEntry e;
            e.wallTime = (int64_t)std::time(nullptr);
            e.timeSec = m.timeSec();
            e.b = h.b;
            e.snrDb = h.snrDb; e.levelDb = m.tel.levelDb; e.cfoHz = (float)h.cfoHz;
            m.tel.log.insert(m.tel.log.begin(), e);
            if (m.tel.log.size() > kSarsatLogMax) m.tel.log.pop_back();
            m.tel.blocksOk++;
            m.tel.bursts++;
            m.tel.dataValid = true;
            m.tel.cfoHz = h.cfoHz;
            m.lastDecodeSec = m.timeSec();
            if (m.log) {
                char line[256];
                snprintf(line, sizeof line, "COSPAS-SARSAT second generation %s %s %s%s", h.b.hexId.c_str(), h.b.beaconType.c_str(), h.b.country.c_str(), h.b.hasPos ? " (position)" : "");
                m.log(line);
            }
            m.publish();
        }
    }
    if (m.timeSec() - m.lastPublish >= 0.25) { m.lastPublish = m.timeSec(); m.publish(); }
}

} // namespace dect2
