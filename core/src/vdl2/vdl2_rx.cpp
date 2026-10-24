// VDL Mode 2 receiver: see vdl2_rx.h.
#include "dect2/vdl2/vdl2_rx.h"
#include "dect2/dsp/baseband_filter.h"
#include "dect2/automation/engine.h"
#include "dect2/results/results_io.h"
#include "dect2/net/feeds.h"
#include "dect2/vdl2/vdl2_proto.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {

using cd = std::complex<double>;
using dect2::math::kPi;
constexpr double kDefaultCenter = 136.85e6;
constexpr int kRingBits = 16;
constexpr size_t kRing = (size_t)1 << kRingBits, kMask = kRing - 1;

// windowed-sinc low-pass (Blackman), unity gain at 0 Hz
std::vector<float> lowpass(int n, double cutoff /* fraction of the sample rate */) {
    if (!(n & 1)) n++;
    std::vector<float> h((size_t)n);
    double sum = 0;
    const int m = n / 2;
    for (int i = 0; i < n; i++) {
        const double k = i - m;
        const double s = k == 0 ? 2 * cutoff : std::sin(2 * kPi * cutoff * k) / (kPi * k);
        const double w = 0.42 - 0.5 * std::cos(2 * kPi * i / (n - 1)) + 0.08 * std::cos(4 * kPi * i / (n - 1));
        h[(size_t)i] = (float)(s * w);
        sum += s * w;
    }
    for (auto& v : h) v = (float)(v / sum);
    return h;
}

// moving sum of D samples (one stage of the decimator), normalised
struct Box {
    std::vector<cf32> buf;
    cd sum = 0;
    size_t pos = 0;
    void init(int d) { buf.assign((size_t)d, cf32(0, 0)); sum = 0; pos = 0; }
    cf32 push(cf32 x) {
        sum += cd(x.real(), x.imag()) - cd(buf[pos].real(), buf[pos].imag());
        buf[pos] = x;
        if (++pos == buf.size()) pos = 0;
        const double g = 1.0 / (double)buf.size();
        return cf32((float)(sum.real() * g), (float)(sum.imag() * g));
    }
};

struct Chan {
    double freqHz = 0, offHz = 0;
    bool inBand = false;
    // mixer
    cd rot = 1, step = 1;
    int renorm = 0;
    // decimator: three moving sums of Da, then FIR h1 every Db-th sample
    Box b1, b2, b3;
    int ca = 0, cb = 0;
    std::vector<cf32> h1buf;      // doubled history for the FIR
    size_t h1pos = 0;
    // stage-1 output ring (fs1) and the burst-search filter output ring
    std::vector<cf32> x1, x2;
    uint64_t n1 = 0, n2 = 0;      // samples written to each
    // burst search
    float floor = 0;
    double blk = 0;
    int blkN = 0;
    uint64_t searchAt = 0;        // next x2 index to test
    bool peakOn = false;
    float peakM = 0;
    uint64_t peakAt = 0, peakEnd = 0;
    cd peakD = 0;
    // a burst being collected
    int state = 0;                // 0 searching, 1 waiting for the header, 2 waiting for the body
    uint64_t bAt = 0;             // training sequence start (x1 index)
    double bCfo = 0;
    uint64_t bNeed = 0;           // x1 index needed before the next step
    int bSyms = 0;
    // statistics
    Vdl2ChannelInfo info;
};

struct DemodOut {
    std::vector<uint8_t> bits;    // descrambled is done later
    float snrDb = 0, levelDb = -120;
    double cfoHz = 0;
};

} // namespace

std::vector<double> vdl2EuropeChannels() { return {136.725e6, 136.775e6, 136.825e6, 136.875e6, 136.900e6, 136.975e6}; }
// every VDL2 channel in use worldwide (Europe, North America, Asia-Pacific): 131.250 .. 131.825 and the 136.650 .. 136.975 block
std::vector<double> vdl2WorldChannels() {
    return {131.250e6, 131.425e6, 131.450e6, 131.475e6, 131.525e6, 131.550e6, 131.725e6, 131.825e6, 136.650e6, 136.700e6, 136.725e6,
            136.775e6, 136.800e6, 136.825e6, 136.850e6, 136.875e6, 136.900e6, 136.925e6, 136.975e6};
}

struct Vdl2Receiver::Impl {
    std::mutex mu;               // settings and telemetry
    double rate = 0, offset = 0, center = 0, tuneErr = 0, pendingTune = 0;
    bool havePending = false, track = true;
    std::vector<double> want = vdl2EuropeChannels();
    bool rebuild = true;
    bool whole = false;                  // Whole span: every channel in use worldwide that lies in the capture, plus the user's list
    std::function<void(const std::string&)> logCb;
    // DSP set-up
    int Da = 1, Db = 1;
    double fs1 = 0, T = 0;        // stage-1 rate, samples per symbol there
    std::vector<float> h1, h2, h3;
    int gd2 = 0;
    std::vector<Chan> ch;
    uint64_t nIn = 0;
    // results
    Vdl2Telemetry tel;
    uint64_t serial = 0, seq = 0;
    double lastGood = -1e9, lastBurst = -1e9;
    float lastSnr = 0;

    void log(const std::string& s) { if (logCb) logCb(s); }
    // mu held: the clock starts again (configure), the kept lists move back with it
    void shiftTimes(double dt) {
        for (auto& m : tel.messages) m.timeSec += dt;
        for (auto& a : tel.aircraft) {
            a.lastHeardSec += dt;
            if (a.hasPos) a.posTime += dt;
            if (a.hasKin) a.kinTime += dt;
            for (auto& p : a.track) p.time += dt;
        }
        for (auto& g : tel.stations) if (g.lastHeardSec != 0) g.lastHeardSec += dt;
        tel.nowSec += dt;
    }

    double effCenter() const { return center > 0 ? center : kDefaultCenter - offset; }

    void setup() {
        rebuild = false;
        ch.clear();
        if (rate < 1) return;
        Da = std::max(1, (int)std::floor(rate / 320e3));
        const double fsa = rate / Da;
        Db = std::max(1, (int)std::floor(fsa / 80e3));
        fs1 = fsa / Db;
        T = fs1 / kVdl2SymbolRate;
        const double stop1 = fs1 - 16.6e3, pass1 = 22e3;
        int n1 = (int)std::ceil(6.6 * fsa / std::max(10e3, stop1 - pass1));
        if (Db == 1) n1 = 1;
        h1 = n1 > 1 ? lowpass(n1, (pass1 + stop1) * 0.5 / fsa * 0.8) : std::vector<float>{1.f};
        h2 = lowpass((int)std::ceil(6.6 * fs1 / 6e3), 15e3 / fs1);     // burst search: pass ~12 kHz, stop ~18 kHz
        gd2 = (int)h2.size() / 2;
        h3 = bbf::overrideTaps("vdl2", lowpass((int)std::ceil(6.6 * fs1 / 4e3), 7.5e3 / fs1), fs1, 15e3);    // channel filter for decoding: pass ~5.8 kHz, stop ~9.2 kHz (cuts some of the
                                                                        // roll-off: less noise, a little ISI; best decoding near the threshold)
        const double c = effCenter();
        const double half = rate * 0.5 - 14e3;
        std::vector<double> f = want.empty() ? vdl2EuropeChannels() : want;
        if (whole) { const auto w = vdl2WorldChannels(); f.insert(f.end(), w.begin(), w.end()); }
        std::sort(f.begin(), f.end());
        f.erase(std::unique(f.begin(), f.end()), f.end());
        for (double fr : f) {
            Chan k;
            k.freqHz = fr;
            k.offHz = fr - c;
            k.inBand = std::fabs(k.offHz) < half;
            k.info.freqHz = fr; k.info.inBand = k.inBand;
            k.b1.init(Da); k.b2.init(Da); k.b3.init(Da);
            k.h1buf.assign(2 * h1.size(), cf32(0, 0));
            k.x1.assign(kRing, cf32(0, 0)); k.x2.assign(kRing, cf32(0, 0));
            if (!k.inBand) { k.x1.clear(); k.x2.clear(); k.x1.shrink_to_fit(); k.x2.shrink_to_fit(); }
            ch.push_back(std::move(k));
        }
        retuneMixers();
        tel.channels.clear();
        for (auto& k : ch) tel.channels.push_back(k.info);
    }

    void retuneMixers() {
        for (auto& k : ch) { const double w = -2 * kPi * (k.offHz + tuneErr) / rate; k.step = cd(std::cos(w), std::sin(w)); }
    }

    // ---------------------------------------------------------------- sample path

    void pushInput(Chan& k, cf32 x) {
        const cd y = cd(x.real(), x.imag()) * k.rot;
        k.rot *= k.step;
        if (++k.renorm == 4096) { k.renorm = 0; k.rot /= std::abs(k.rot); }
        cf32 v((float)y.real(), (float)y.imag());
        if (Da > 1) {
            v = k.b3.push(k.b2.push(k.b1.push(v)));
            if (++k.ca < Da) return;
            k.ca = 0;
        }
        const size_t L = h1.size();
        k.h1buf[k.h1pos] = v; k.h1buf[k.h1pos + L] = v;
        k.h1pos = (k.h1pos + 1) % L;
        if (++k.cb < Db) return;
        k.cb = 0;
        cf32 acc(0, 0);
        const cf32* hb = &k.h1buf[k.h1pos];
        for (size_t i = 0; i < L; i++) acc += hb[i] * h1[i];
        k.x1[k.n1 & kMask] = acc;
        k.n1++;
        // burst-search filter: x2[i] is centred on x1[i]
        if (k.n1 >= h2.size()) {
            const uint64_t i = k.n1 - 1 - (uint64_t)gd2;
            cf32 a2(0, 0);
            const uint64_t b0 = k.n1 - h2.size();
            for (size_t j = 0; j < h2.size(); j++) a2 += k.x1[(b0 + j) & kMask] * h2[j];
            k.x2[i & kMask] = a2;
            k.n2 = i + 1;
            // noise floor: block means, falling fast, rising slowly
            k.blk += std::norm(a2);
            if (++k.blkN == 512) {
                const float m = (float)(k.blk / 512);
                if (k.floor <= 0) k.floor = m;
                else if (m < k.floor) k.floor = 0.7f * k.floor + 0.3f * m;
                else k.floor *= 1.003f;
                k.blk = 0; k.blkN = 0;
            }
        }
    }

    // ---------------------------------------------------------------- burst search on x2

    void beginBurst(Chan& k) {
        k.state = 1;
        k.bAt = k.peakAt;
        k.bCfo = std::arg(k.peakD) * kVdl2SymbolRate / (2 * kPi);
        k.bNeed = k.bAt + (uint64_t)std::ceil((kVdl2PreambleSyms + 10) * T) + h3.size() + 8;
        k.info.bursts++;
    }

    void search(Chan& k) {
        const int span = (int)std::ceil(15 * T) + 1;
        constexpr float kDetThr = 0.56f, kDetDrop = 0.88f;
        while (k.state == 0 && k.searchAt + (uint64_t)span < k.n2) {
            const uint64_t t = k.searchAt;
            if ((double)t < 10 * T + (double)h3.size()) { k.searchAt++; continue; }
            if (k.n2 - t > kRing - 4096) { k.searchAt = k.n2 - 1024; k.peakOn = false; continue; }   // fell behind
            cf32 s[kVdl2PreambleSyms];
            double e = 0;
            for (int q = 0; q < kVdl2PreambleSyms; q++) { s[q] = k.x2[(t + (uint64_t)std::lround(q * T)) & kMask]; e += std::norm(s[q]); }
            e /= kVdl2PreambleSyms;
            float M = 0;
            cd D = 0;
            double eh = 0, et = 0;                       // the energy must fill the whole training sequence, not only its start or end
            for (int q = 0; q < 4; q++) { eh += std::norm(s[q]); et += std::norm(s[kVdl2PreambleSyms - 1 - q]); }
            const bool ener = k.floor > 0 && e > 3.0 * k.floor && std::min(eh, et) > 4 * 0.3 * e;
            if (ener) {
                for (int q = 0; q + 1 < kVdl2PreambleSyms; q++) {
                    const double dp = (kVdl2PreamblePhase[q + 1] - kVdl2PreamblePhase[q]) * kPi / 4;
                    D += cd(s[q + 1].real(), s[q + 1].imag()) * std::conj(cd(s[q].real(), s[q].imag())) * cd(std::cos(-dp), std::sin(-dp));
                }
                M = (float)(std::abs(D) / (e * (kVdl2PreambleSyms - 1)));
            }
            if (ener && M >= kDetThr) {
                if (!k.peakOn || M >= k.peakM) {
                    if (!k.peakOn) k.peakEnd = t + (uint64_t)std::ceil((kVdl2PreambleSyms + 2) * T);
                    k.peakOn = true;
                    k.peakM = M;
                    k.peakAt = t;
                    k.peakD = D;
                }
            }
            if (k.peakOn && (t + 1 >= k.peakEnd || (M < k.peakM * kDetDrop && M < kDetThr))) {
                k.peakOn = false;
                if (k.peakM >= kDetThr) beginBurst(k);
            }
            k.searchAt++;
        }
    }

    // ---------------------------------------------------------------- demodulation of a found burst

    // the channel-filtered, carrier-corrected samples from x1 index a over n samples
    void segment(const Chan& k, uint64_t a, size_t n, double cfo, std::vector<cf32>& y) {
        const size_t L = h3.size(), m = L / 2;
        std::vector<cf32> z(n + L);
        const double w = -2 * kPi * cfo / fs1;
        for (size_t i = 0; i < n + L; i++) {
            const uint64_t idx = a + i - m;
            const double ph = w * (double)((int64_t)i - (int64_t)m);
            z[i] = k.x1[idx & kMask] * cf32((float)std::cos(ph), (float)std::sin(ph));
        }
        y.assign(n, cf32(0, 0));
        for (size_t i = 0; i < n; i++) { cf32 acc(0, 0); for (size_t j = 0; j < L; j++) acc += z[i + j] * h3[j]; y[i] = acc; }
    }

    static cf32 interp(const std::vector<cf32>& y, double t) {
        const int64_t i = (int64_t)std::floor(t);
        const float f = (float)(t - (double)i);
        if (i < 1 || i + 2 >= (int64_t)y.size()) return cf32(0, 0);
        const cf32 a = y[(size_t)i - 1], b = y[(size_t)i], c = y[(size_t)i + 1], d = y[(size_t)i + 2];
        // cubic Lagrange
        const float c0 = -f * (f - 1) * (f - 2) / 6, c1 = (f + 1) * (f - 1) * (f - 2) / 2, c2 = -(f + 1) * f * (f - 2) / 2, c3 = (f + 1) * f * (f - 1) / 6;
        return a * c0 + b * c1 + c * c2 + d * c3;
    }

    // demodulates nSym symbols after the training sequence; returns false when the training sequence does not fit well enough
    bool demod(const Chan& k, int nSym, DemodOut& o) {
        const int pre = 6;                                       // symbols kept before the training sequence
        const uint64_t a = k.bAt - (uint64_t)std::ceil(pre * T);
        const size_t n = (size_t)std::ceil((pre + kVdl2PreambleSyms + nSym + 3) * T);
        std::vector<cf32> y;
        segment(k, a, n, k.bCfo, y);
        const double t0 = (double)(k.bAt - a);
        // fine timing on the training symbols
        double bestT = t0, bestV = -1;
        cf32 ref[kVdl2PreambleSyms];
        for (int q = 0; q < kVdl2PreambleSyms; q++) { const double p = kVdl2PreamblePhase[q] * kPi / 4; ref[q] = cf32((float)std::cos(p), (float)-std::sin(p)); }
        for (double d = -3.0; d <= 3.0001; d += 0.125) {
            cf32 acc(0, 0);
            for (int q = 0; q < kVdl2PreambleSyms; q++) acc += interp(y, t0 + d + q * T) * ref[q];
            if (std::norm(acc) > bestV) { bestV = std::norm(acc); bestT = t0 + d; }
        }
        cd z[kVdl2PreambleSyms];
        double e = 0;
        for (int q = 0; q < kVdl2PreambleSyms; q++) { const cf32 v = interp(y, bestT + q * T) * ref[q]; z[q] = cd(v.real(), v.imag()); e += std::norm(z[q]); }
        cd D = 0;
        for (int q = 0; q + 1 < kVdl2PreambleSyms; q++) D += z[q + 1] * std::conj(z[q]);
        const double dr = std::arg(D);
        cd P = 0;
        for (int q = 0; q < kVdl2PreambleSyms; q++) P += z[q] * std::polar(1.0, -dr * q);
        if (std::abs(D) < 0.5 * e) return false;
        o.cfoHz = k.bCfo + dr * kVdl2SymbolRate / (2 * kPi);
        const double amp = std::abs(P) / kVdl2PreambleSyms;
        // phase locked loop and timing loop over the whole burst
        double th = std::arg(P), om = dr, tau = bestT;
        const double ka = 0.12, kb = 0.004, kt = 0.02;
        int prevIdx = 0;
        double sig = 0, err2 = 0;
        cf32 prevS = interp(y, tau - T);
        o.bits.clear();
        o.bits.reserve((size_t)nSym * 3);
        const int total = kVdl2PreambleSyms + nSym;
        for (int q = 0; q < total; q++) {
            const double tq = tau + q * T;
            const cf32 sv = interp(y, tq);
            const cd r = cd(sv.real(), sv.imag()) * std::polar(1.0, -th);
            int idx;
            if (q < kVdl2PreambleSyms) idx = ((kVdl2PreamblePhase[q] % 8) + 8) % 8;
            else { idx = (int)std::lround(std::arg(r) / (kPi / 4)); idx = ((idx % 8) + 8) % 8; }
            const cd dec = std::polar(1.0, idx * kPi / 4);
            const double pe = std::arg(r * std::conj(dec));
            om += kb * pe;
            th += om + ka * pe;
            sig += std::norm(r);
            err2 += std::norm(r - amp * dec);
            // Gardner timing error (normalised)
            const cf32 mid = interp(y, tq - T * 0.5);
            const double ge = std::real((cd(prevS.real(), prevS.imag()) - cd(sv.real(), sv.imag())) * std::conj(cd(mid.real(), mid.imag())));
            if (q > 2) tau += std::max(-0.05, std::min(0.05, kt * ge / std::max(1e-12, amp * amp))) * T * 0.1;
            prevS = sv;
            if (q >= kVdl2PreambleSyms) {
                const int stp = ((idx - prevIdx) % 8 + 8) % 8;
                const int b = kVdl2GrayBits[stp];
                o.bits.push_back((uint8_t)((b >> 2) & 1)); o.bits.push_back((uint8_t)((b >> 1) & 1)); o.bits.push_back((uint8_t)(b & 1));
            }
            prevIdx = idx;
        }
        o.snrDb = (float)(10 * std::log10(std::max(1e-12, sig) / std::max(1e-12, err2)));
        o.levelDb = (float)(10 * std::log10(std::max(1e-20, amp * amp)));
        return true;
    }

    // ---------------------------------------------------------------- per-channel state machine

    void process(Chan& k) {
        search(k);
        if (k.state == 1 && k.n1 >= k.bNeed) {
            DemodOut o;
            uint32_t len = 0;
            bool corr = false;
            Vdl2Layout l;
            bool ok = demod(k, 9, o);
            if (ok) {
                std::vector<uint8_t> b(o.bits.begin(), o.bits.begin() + kVdl2HeaderBits);
                vdl2Scramble(b, 0);
                uint32_t w = 0;
                for (int i = 0; i < kVdl2HeaderBits; i++) w = (w << 1) | b[(size_t)i];
                ok = vdl2HeaderDecode(w, len, corr) && vdl2Layout(len, l);
            }
            if (!ok) { fail(k, false, 1); return; }
            const int bits = kVdl2HeaderBits + 8 * (l.dataOct + l.fecOct);
            k.bSyms = (bits + 2) / 3;
            k.bNeed = k.bAt + (uint64_t)std::ceil((kVdl2PreambleSyms + k.bSyms + 4) * T) + h3.size() + 8;
            if ((double)(k.bNeed - k.bAt) > (double)kRing - 8192) { fail(k, false, 2); return; }
            k.state = 2;
        }
        if (k.state == 2 && k.n1 >= k.bNeed) {
            DemodOut o;
            if (!demod(k, k.bSyms, o)) { fail(k, true, 3); return; }
            std::vector<uint8_t> b = o.bits;
            vdl2Scramble(b, 0);
            uint32_t w = 0, len = 0;
            bool corr = false;
            for (int i = 0; i < kVdl2HeaderBits; i++) w = (w << 1) | b[(size_t)i];
            Vdl2Layout l;
            if (!vdl2HeaderDecode(w, len, corr) || !vdl2Layout(len, l) || b.size() < (size_t)(kVdl2HeaderBits + 8 * (l.dataOct + l.fecOct))) { fail(k, true, 4); return; }
            std::vector<uint8_t> data;
            int fixed = 0;
            if (!vdl2BurstData(b.data() + kVdl2HeaderBits, l, len, data, fixed)) { fail(k, true, 5); return; }
            const auto frames = vdl2HdlcFrames(data);
            int good = 0, bad = 0;
            const double tSec = (double)k.bAt / fs1;
            for (const auto& f : frames) {
                if (f.size() < 11 || !vdl2FcsOk(f.data(), f.size())) { bad++; continue; }
                Vdl2Message m;
                if (!vdl2ParseFrame(f.data(), f.size() - 2, m)) { bad++; continue; }
                m.timeSec = tSec; m.wallTime = (int64_t)time(nullptr); m.freqHz = k.freqHz; m.levelDb = o.levelDb; m.snrDb = o.snrDb; m.cfoHz = (float)(o.cfoHz + tuneErr); m.rsFixed = fixed;
                good++;
                addMessage(std::move(m), corr, fixed, good == 1);
            }
            {
                std::lock_guard<std::mutex> lk(mu);
                tel.fcsBad += (uint64_t)bad;
            }
            if (!good) { fail(k, true, 6); return; }
            k.info.good++;
            k.info.frames += (uint64_t)good;
            k.info.lastHeardSec = tSec;
            k.info.levelDb = o.levelDb; k.info.snrDb = o.snrDb; k.info.lastCfoHz = (float)(o.cfoHz + tuneErr);
            lastGood = tSec; lastSnr = o.snrDb;
            if (track) { std::lock_guard<std::mutex> lk(mu); pendingTune = tuneErr + 0.3 * std::max(-6000.0, std::min(6000.0, o.cfoHz)); havePending = true; }
            k.state = 0;
            k.searchAt = k.bAt + (uint64_t)std::floor((kVdl2PreambleSyms + k.bSyms) * T);
            k.peakOn = false;
        }
    }

    void fail(Chan& k, bool counted, int why) {
        static const bool dbg = getenv("DECT2_VDL2_DEBUG") != nullptr;
        if (dbg) fprintf(stderr, "vdl2 fail %d at %.4f s on %.3f MHz (cfo %.0f)\n", why, (double)k.bAt / fs1, k.freqHz / 1e6, k.bCfo);
        if (counted) { k.info.bad++; std::lock_guard<std::mutex> lk(mu); tel.blocksBad++; }
        k.state = 0;
        k.searchAt = std::max(k.searchAt, k.bAt + (uint64_t)std::ceil((kVdl2PreambleSyms + 4) * T));
        k.peakOn = false;
    }

    // ---------------------------------------------------------------- bookkeeping

    void addMessage(Vdl2Message&& m, bool headerFixed, int fixed, bool firstOfBurst) {
        std::lock_guard<std::mutex> lk(mu);
        m.serial = ++serial;
        tel.blocksOk++;
        if (firstOfBurst) { tel.rsFixedTotal += (uint64_t)fixed; if (headerFixed) tel.headerFixed++; }
        if (m.hasPos) tel.positionsTotal++;
        // aircraft: the source when it is one, else the destination when it is one
        const bool srcAir = m.srcType == 1, dstAir = m.dstType == 1;
        if (srcAir || dstAir) {
            const uint32_t a = srcAir ? m.src : m.dst;
            auto it = std::find_if(tel.aircraft.begin(), tel.aircraft.end(), [&](const Vdl2Aircraft& x) { return x.icao == a; });
            Vdl2Aircraft c;
            if (it != tel.aircraft.end()) { c = *it; tel.aircraft.erase(it); }
            c.icao = a;
            c.messages++;
            c.freqHz = m.freqHz; c.lastHeardSec = m.timeSec; c.lastWall = m.wallTime;
            if (srcAir) { c.levelDb = m.levelDb; c.onGround = m.onGround; if (m.dstType == 4 || m.dstType == 5) c.groundStation = m.dst; }
            else if (m.srcType == 4 || m.srcType == 5) c.groundStation = m.src;
            if (!m.reg.empty()) c.reg = m.reg;
            if (!m.flight.empty() && srcAir) c.flight = m.flight;
            if (m.hasKin && srcAir) {
                c.hasKin = true;
                c.groundKt = m.groundKt;
                c.vsFpm = m.vsFpm;
                c.kinTime = m.timeSec;
                c.hasTrack = true;
                c.trackDeg = m.kinTrackDeg;
            }
            if (m.hasPos && srcAir) {
                if (c.hasPos) {
                    const double p1 = c.lat * kPi / 180, p2 = m.lat * kPi / 180, dl = (m.lon - c.lon) * kPi / 180;
                    const double y = std::sin(dl) * std::cos(p2), x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
                    if (std::hypot(m.lat - c.lat, (m.lon - c.lon) * std::cos(p2)) > 0.01) {
                        c.hasTrack = true;
                        c.trackDeg = std::fmod(std::atan2(y, x) * 180 / kPi + 360, 360);
                    }
                }
                c.hasPos = true; c.lat = m.lat; c.lon = m.lon; c.posKind = m.posKind; c.posTime = m.timeSec;
                if (m.hasAlt) { c.hasAlt = true; c.altFt = m.altFt; }
                c.track.push_back({m.lat, m.lon, m.hasAlt ? m.altFt : c.altFt, m.timeSec});
                if (c.track.size() > 24) c.track.erase(c.track.begin());
                if (!c.hasKin && c.track.size() >= 2) {
                    const Vdl2TrackPoint& p0 = c.track[c.track.size() - 2];
                    const Vdl2TrackPoint& p1 = c.track.back();
                    const double seg = p1.time - p0.time;
                    if (seg > 3.0 && seg < 7200.0) {
                        const double kt = vdl2DistNm(p0.lat, p0.lon, p1.lat, p1.lon) / (seg / 3600.0);
                        if (kt >= 15.0) { c.hasKin = true; c.groundKt = kt; c.kinTime = m.timeSec; }
                    }
                }
            }
            tel.aircraft.insert(tel.aircraft.begin(), std::move(c));
            if (tel.aircraft.size() > 200) tel.aircraft.pop_back();
        }
        if (m.srcType == 4 || m.srcType == 5 || m.dstType == 4 || m.dstType == 5) {
            const bool src = m.srcType == 4 || m.srcType == 5;
            const uint32_t a = src ? m.src : m.dst;
            auto it = std::find_if(tel.stations.begin(), tel.stations.end(), [&](const Vdl2GroundStation& x) { return x.addr == a; });
            Vdl2GroundStation g;
            if (it != tel.stations.end()) { g = *it; tel.stations.erase(it); }
            g.addr = a; g.type = src ? m.srcType : m.dstType;
            if (src) {
                g.messages++; g.freqHz = m.freqHz; g.lastHeardSec = m.timeSec; g.levelDb = m.levelDb;
                if (m.hasPos && m.posKind == "XID ground station location") { g.hasPos = true; g.lat = m.lat; g.lon = m.lon; }
                const size_t at = m.detail.find("airport coverage ");
                if (at != std::string::npos) g.airports = m.detail.substr(at + 17, m.detail.find('\n', at) - at - 17);
            }
            tel.stations.insert(tel.stations.begin(), std::move(g));
            if (tel.stations.size() > 64) tel.stations.pop_back();
        }
        if (Feeds::get().wantVdl2()) Feeds::get().vdl2(m);   // the data feed out (feeds.h)
        if (automation::wants(automation::FVdl2)) {   // the automations (automation/engine.h)
            automation::Event ev("vdl2.message");
            char a[8], b[8];
            snprintf(a, sizeof a, "%06X", m.src & 0xFFFFFF); snprintf(b, sizeof b, "%06X", m.dst & 0xFFFFFF);
            ev.at(automation::wallNow(), "VDL2", m.freqHz);
            ev.str("src", a).str("dst", b).flag("from_air", m.fromAir).str("proto", m.proto).str("summary", m.summary).str("frame", m.frameName);
            automation::emit(std::move(ev));
        }
        tel.messages.insert(tel.messages.begin(), std::move(m));
        if (tel.messages.size() > kVdl2MsgMax) tel.messages.pop_back();
        seq++;
    }
};

Vdl2Receiver::Vdl2Receiver() : p_(new Impl) {}
Vdl2Receiver::~Vdl2Receiver() = default;

void Vdl2Receiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->rebuild = true; p_->nIn = 0; p_->shiftTimes(-p_->tel.nowSec); }
void Vdl2Receiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (p_->offset != hz) { p_->offset = hz; p_->rebuild = true; } }
void Vdl2Receiver::setCenterHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (p_->center != hz) { p_->center = hz; p_->rebuild = true; } }
bool Vdl2Receiver::ready() const { return p_->rate >= vdl2Tuning().minSampleRate - 1; }
void Vdl2Receiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->logCb = std::move(cb); }
void Vdl2Receiver::setWholeSpan(bool on) { std::lock_guard<std::mutex> lk(p_->mu); if (on != p_->whole) { p_->whole = on; p_->rebuild = true; } }
void Vdl2Receiver::setChannels(const std::vector<double>& hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->want) { p_->want = hz; p_->rebuild = true; } }
void Vdl2Receiver::setTuneErrorHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->pendingTune = hz; p_->havePending = true; }
void Vdl2Receiver::setTrackTuning(bool on) { std::lock_guard<std::mutex> lk(p_->mu); p_->track = on; }

void Vdl2Receiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    Vdl2Telemetry t;                     // the messages, aircraft and ground stations stay (clearResults); the clock runs on
    t.centerHz = p_->tel.centerHz; t.nowSec = p_->tel.nowSec;
    t.messages = std::move(p_->tel.messages); t.aircraft = std::move(p_->tel.aircraft); t.stations = std::move(p_->tel.stations);
    p_->tel = std::move(t);
    p_->lastGood = p_->lastBurst = -1e9;
    p_->rebuild = true;
    p_->seq++;
}

void Vdl2Receiver::feed(const cf32* x, size_t n) {
    Impl& I = *p_;
    {
        std::lock_guard<std::mutex> lk(I.mu);
        if (I.rebuild) I.setup();
        bool busy = false;
        for (auto& k : I.ch) busy |= k.state != 0;
        if (I.havePending && !busy) { I.tuneErr = I.pendingTune; I.havePending = false; I.retuneMixers(); }
    }
    if (I.rate < 1 || I.ch.empty()) { I.nIn += n; return; }
    for (auto& k : I.ch) {
        if (!k.inBand) continue;
        for (size_t i = 0; i < n; i++) {
            cf32 v = x[i];
            if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
            I.pushInput(k, v);
        }
        for (int it = 0; it < 10000; it++) {          // every burst the new samples complete
            const uint64_t at = k.searchAt;
            const int st = k.state;
            I.process(k);
            if (k.state == st && k.searchAt == at) break;
        }
    }
    I.nIn += n;
    std::lock_guard<std::mutex> lk(I.mu);
    const double now = (double)I.nIn / I.rate;
    I.tel.nowSec = now;
    I.tel.inputRate = I.rate; I.tel.channelRate = I.fs1;
    I.tel.centerHz = I.effCenter();
    I.tel.cfoHz = I.tuneErr;
    I.tel.snrDb = I.lastSnr;
    I.tel.dataValid = I.tel.blocksOk > 0;
    uint64_t bursts = 0;
    I.tel.channels.clear();
    for (auto& k : I.ch) {
        k.info.active = k.info.lastHeardSec > 0 && now - k.info.lastHeardSec < 2.0;
        k.info.noiseDb = k.floor > 0 ? (float)(10 * std::log10(k.floor)) : -120.f;
        bursts += k.info.bursts;
        I.tel.channels.push_back(k.info);
    }
    I.tel.state = now - I.lastGood < 30 ? 2 : bursts ? 1 : 0;
    I.seq++;
}

void Vdl2Receiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->tel.messages.clear(); p_->tel.aircraft.clear(); p_->tel.stations.clear();
    p_->seq++;
}
std::string Vdl2Receiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->tel.nowSec, p_->serial, p_->tel.messages, p_->tel.aircraft, p_->tel.stations);
}
bool Vdl2Receiver::loadResults(const std::string& blob) {
    double t0 = 0;
    uint64_t serial = 0;
    Vdl2Telemetry l;
    if (!results::unpack(blob, t0, serial, l.messages, l.aircraft, l.stations)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    const double now = p_->tel.nowSec;
    p_->tel.messages = std::move(l.messages); p_->tel.aircraft = std::move(l.aircraft); p_->tel.stations = std::move(l.stations);
    p_->serial = std::max(p_->serial, serial);
    p_->tel.nowSec = t0;
    p_->shiftTimes(now - t0);
    p_->seq++;
    return true;
}

bool Vdl2Receiver::telemetry(Vdl2Telemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->seq == lastSeq) return false;
    out = p_->tel;
    out.seq = p_->seq;
    return true;
}

ModeTuning vdl2Tuning() {
    ModeTuning t;
    t.stdMode = 29; t.id = "vdl2"; t.name = "VDL Mode 2";
    t.minMhz = 118; t.maxMhz = 140; t.defMhz = 136.850;
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.4;                    // 136.725 - 136.975 MHz around 136.85
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 0;                      // 136.850 is not a VDL2 channel: the DC spike falls between two
    return t;
}

} // namespace dect2
