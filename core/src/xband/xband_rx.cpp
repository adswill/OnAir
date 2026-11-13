// See xband_rx.h. Link parameters from SatDump's pipelines (GPL-3.0, Copyright Aang23 and contributors); code written for OnAir.
#include "dect2/common/bits.h"
#include "dect2/xband/xband_rx.h"
#include "dect2/xband/xband_proto.h"
#include "dect2/xband/xband_products.h"
#include "dect2/common/mathconst.h"
#include "dect2/dsp/dsp_compat.h"
#include "dect2/dsp/fir_dot.h"
#include "dect2/dsp/fast_sincos.h"
#include "dect2/dsp/rrc.h"
#include "dect2/hrpt/hrpt_proto.h"
#include "dect2/wxsat/wxsat_proto.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace dect2 {

namespace {
using math::kPi;
using cfp = std::complex<float>;
double nowSec() { using namespace std::chrono; return duration<double>(steady_clock::now().time_since_epoch()).count(); }
inline int8_t sat8(float v) { return (int8_t)std::clamp((int)std::lround(v), -127, 127); }

void loopGains(double bl, double zeta, double& a, double& b) {
    const double w = bl / (zeta + 1.0 / (4 * zeta));
    const double d = 1 + 2 * zeta * w + w * w;
    a = 4 * zeta * w / d; b = 4 * w * w / d;
}

// a * b for finite values, the products apart as std::complex does them (the same result, without its NaN check every sample)
inline cfp cmulFinite(cfp a, cfp b) {
    const float ac = a.real() * b.real(), bd = a.imag() * b.imag(), ad = a.real() * b.imag(), bc = a.imag() * b.real();
    return {ac - bd, ad + bc};
}

// Decimating FIR, complex data, doubled ring; the dot product through firDot (sixteen accumulators the compiler vectorises)
struct FirDec {
    std::vector<float> taps, h2;
    std::vector<cfp> ring;
    int pos = 0, n = 0, dec = 1, cnt = 0;
    void design(int ratio, double cutoffNorm) {
        dec = std::max(1, ratio);
        n = std::max(3, 12 * dec + 1);
        taps.assign((size_t)n, 0);
        double sum = 0;
        for (int i = 0; i < n; i++) {
            const double t = i - (n - 1) / 2.0;
            const double s = t == 0 ? 2 * cutoffNorm : std::sin(2 * kPi * cutoffNorm * t) / (kPi * t);
            taps[(size_t)i] = (float)(s * (0.54 - 0.46 * std::cos(2 * kPi * i / (n - 1))));
            sum += taps[(size_t)i];
        }
        for (auto& v : taps) v /= (float)sum;
        h2 = firDoubleTaps(taps.data(), n);
        ring.assign((size_t)n * 2, cfp(0, 0));
        pos = 0; cnt = 0;
    }
    void clear() { std::fill(ring.begin(), ring.end(), cfp(0, 0)); pos = 0; cnt = 0; }
    bool push(cfp x, cfp& out) {
        pos = (pos == 0 ? n : pos) - 1;
        ring[(size_t)pos] = ring[(size_t)pos + n] = x;
        if (++cnt < dec) return false;
        cnt = 0;
        out = firDot(h2.data(), &ring[(size_t)pos], n);
        return true;
    }
};

struct Rrc {
    std::vector<float> h, h2;
    std::vector<cfp> ring;
    int n = 0, pos = 0;
    void design(double sps, double alpha) {
        const int half = std::max(4, (int)std::ceil(4 * sps));
        n = 2 * half + 1;
        h.resize((size_t)n);
        double e = 0;
        for (int i = 0; i < n; i++) { h[(size_t)i] = (float)dsp::rrc((i - half) / sps, alpha); e += h[(size_t)i] * h[(size_t)i]; }
        for (auto& v : h) v /= (float)std::sqrt(e);
        h2 = firDoubleTaps(h.data(), n);
        ring.assign((size_t)n * 2, cfp(0, 0));
        pos = 0;
    }
    void clear() { std::fill(ring.begin(), ring.end(), cfp(0, 0)); pos = 0; }
    cfp push(cfp x) {
        pos = (pos == 0 ? n : pos) - 1;
        ring[(size_t)pos] = ring[(size_t)pos + n] = x;
        return firDot(h2.data(), &ring[(size_t)pos], n);
    }
};

class Demod {
public:
    double inRate = 0, sym = 0, offsetHz = 0, decRate = 0;
    XbMod mod = kXbQpsk;
    bool acquired = false;
    double cfoCoarse = 0;
    float agc = 1, carrierLock = 0;
    // the SNR from the spread of the symbol magnitudes, worked out when it is reported (not a log10 every symbol)
    float snrDb() const { return 10.f * (float)std::log10(std::max(1e-3f, meanAbs_ * meanAbs_ / std::max(1e-6f, 2 * varAbs_))); }
    std::vector<std::pair<float, float>> cons;
    size_t consPos = 0;

    void configure(double rate, double symbolRate, XbMod m, double alpha) {
        inRate = rate; sym = symbolRate; mod = m; alpha_ = alpha;
        int ratio = (int)std::floor(rate / (4.0 * sym));
        if (ratio < 2) ratio = 1;
        ratio_ = ratio;
        decRate = rate / ratio;
        if (ratio > 1) fir_.design(ratio, 0.5 / ratio * 0.95);
        spsNom_ = decRate / sym;
        rrc_.design(spsNom_, alpha);
        loopGains(m == kXbBpsk ? 0.008 : 0.005, 0.707, cA_, cB_);
        tBl_ = 0.006;
        loopGains(tBl_, 0.9, tA_, tB_);
        resetAll();
    }
    void setOffset(double hz) { offsetHz = hz; updateStep(); }
    void resetAll() {
        fir_.clear(); rrc_.clear();
        rot_ = 1; for (auto& h : h_) h = 0;
        acquired = false; acq_.clear(); acq_.reserve(kAcq); cfoCoarse = 0; idle = false; skipLeft_ = 0; updateStep();
        phase_ = 0; freq_ = 0; t_ = spsNom_ / 2; strobePh_ = 0; lastStrobe_ = 0; mid_ = 0; prevMid_ = 0; sps_ = spsNom_;
        agc = 0.1f; carrierLock = 0; meanAbs_ = 0; varAbs_ = 0;
        cons.clear(); consPos = 0;
    }
    void reacquire() { acquired = false; acq_.clear(); idle = false; skipLeft_ = 0; }
    double currentCfo() const { return cfoCoarse + freq_ * decRate / (2 * kPi); }

    // The pre-check: with no carrier line in a search block (the power-law spectrum of noise is flat) the receiver looks at one block of
    // every kIdleEvery and skips the rest, until a line shows. A signal is found within kIdleEvery blocks (8 x 131072 samples: 0.07 s at 15 Msps).
    static constexpr int kIdleEvery = 8;
    // Measured on the test signals (all profiles, 0.15 s, CFO 20 kHz): a signal's line is 40 or more at 3 dB Es/N0 (120-13000 on the links
    // that still decode there); white noise reached 11.7 at most (the OQPSK and narrow links: their filtered noise is not flat).
    // A noise block over the threshold only costs what every block cost before.
    static constexpr double kGatePeak = 16.0;
    float lastPeak = 0;
    bool idle = false;
    uint64_t skipped = 0;

    template<class S> void feed(const cf32* x, size_t n, S&& sink) {
        for (size_t i = 0; i < n;) {
            if (skipLeft_ > 0) { const size_t k = (size_t)std::min<uint64_t>(skipLeft_, n - i); skipLeft_ -= k; skipped += k; i += k; continue; }
            rot_ = cmulFinite(rot_, step_);
            if (((++renorm_) & 4095) == 0) rot_ /= std::abs(rot_);
            cfp v = cmulFinite(cfp(x[i].real(), x[i].imag()), rot_);
            i++;
            cfp y;
            if (ratio_ == 1) y = v; else if (!fir_.push(v, y)) continue;
            sample(y, sink);
        }
    }

private:
    static constexpr int kAcq = 1 << 17;
    uint64_t skipLeft_ = 0;
    std::vector<cfp> acq_;
    FirDec fir_;
    Rrc rrc_;
    int ratio_ = 1;
    double alpha_ = 0.5;
    double spsNom_ = 2, sps_ = 2, cA_ = 0.01, cB_ = 1e-4, tBl_ = 0.005, tA_ = 0, tB_ = 0;
    double phase_ = 0, freq_ = 0, t_ = 1;
    cfp rot_{1, 0}, step_{1, 0};
    uint32_t renorm_ = 0;
    cfp h_[4];
    int strobePh_ = 0;
    cfp lastStrobe_{0, 0}, mid_{0, 0}, prevMid_{0, 0};
    float meanAbs_ = 0, varAbs_ = 0;
    void updateStep() {
        const double w = -2 * kPi * (offsetHz + cfoCoarse) / inRate;
        step_ = cfp((float)std::cos(w), (float)std::sin(w));
    }

    // Peak of a power spectrum over carrier bins, for the power law (mult) and optional doublet spacing (side, bins). Returns the peak to mean ratio and the offset in Hz.
    double scan(int mult, int side, double& offHz, const std::vector<cfp>* use = nullptr, int log2nIn = 15, double rateIn = 0) {
        const int log2n = log2nIn, N = 1 << log2n;
        const std::vector<cfp>& src = use ? *use : acq_;
        const double decRate2 = rateIn > 0 ? rateIn : decRate;
        std::vector<float> re((size_t)N), im((size_t)N), pow((size_t)N);
        std::fill(pow.begin(), pow.end(), 0.f);
        const int blocks = (int)(src.size() / (size_t)N);
        for (int b = 0; b < std::max(1, blocks); b++) {
            for (int i = 0; i < N; i++) {
                cfp v = src[(size_t)(b * N + i)];
                cfp w = mult == 4 ? (v * v) * (v * v) : v * v;
                w /= std::max(1e-7f, std::abs(w));
                const float win = 0.5f - 0.5f * (float)std::cos(2 * kPi * i / N);
                re[(size_t)i] = w.real() * win; im[(size_t)i] = w.imag() * win;
            }
            fftSplit(re.data(), im.data(), log2n, false);
            for (int i = 0; i < N; i++) pow[(size_t)i] += re[(size_t)i] * re[(size_t)i] + im[(size_t)i] * im[(size_t)i];
        }
        const double maxHz = std::min(0.45 * decRate2 / mult, 1.5e6);
        const int maxBin = (int)(maxHz * mult / decRate2 * N);
        int best = 0; float bv = 0;
        double total = 0;
        const auto metric = [&](int k) { return side == 0 ? pow[(size_t)((k + 2 * N) % N)] : pow[(size_t)((k + side + 2 * N) % N)] + pow[(size_t)((k - side + 2 * N) % N)]; };
        for (int k = -maxBin; k <= maxBin; k++) { const float v = metric(k); total += v; if (v > bv) { bv = v; best = k; } }
        const double peak = bv / std::max(1e-9, total / (2 * maxBin + 1));
        const float l = metric(best - 1), c = bv, r = metric(best + 1);
        const double d = 2 * c - l - r;
        const double delta = d > 0 ? 0.5 * (r - l) / d : 0;
        offHz = (best + delta) * decRate2 / N / mult;
        return peak;
    }

    void acquire() {
        // a matched filter first: at many samples a symbol the power law works on the noise of the whole band otherwise
        { Rrc f; f.design(spsNom_, alpha_); for (auto& v : acq_) v = f.push(v); }
        double off = 0;
        double peak = 0;
        const int N = 1 << 15;
        if (mod == kXbBpsk) peak = scan(2, 0, off);
        else if (mod == kXbQpsk) peak = scan(4, 0, off);
        else {
            double o4 = 0, o2 = 0;
            double p4;
            if (spsNom_ < 3) {
                // at about 2 samples a symbol the fourth power of OQPSK has almost no line: interpolate by two first
                std::vector<float> re((size_t)N), im((size_t)N), re2((size_t)2 * N), im2((size_t)2 * N);
                for (int i = 0; i < N; i++) { re[(size_t)i] = acq_[(size_t)i].real(); im[(size_t)i] = acq_[(size_t)i].imag(); }
                fftSplit(re.data(), im.data(), 15, false);
                for (int i = 0; i < N / 2; i++) { re2[(size_t)i] = re[(size_t)i]; im2[(size_t)i] = im[(size_t)i]; re2[(size_t)(2 * N - N / 2 + i)] = re[(size_t)(N / 2 + i)]; im2[(size_t)(2 * N - N / 2 + i)] = im[(size_t)(N / 2 + i)]; }
                fftSplit(re2.data(), im2.data(), 16, true);
                std::vector<cfp> up((size_t)2 * N);
                for (int i = 0; i < 2 * N; i++) up[(size_t)i] = cfp(re2[(size_t)i], im2[(size_t)i]);
                p4 = scan(4, 0, o4, &up, 16, decRate * 2);
            } else p4 = scan(4, 0, o4);
            const double p2 = scan(2, (int)std::lround(sym * 0.5 / decRate * N), o2);
            peak = p4 >= p2 ? p4 : p2; off = p4 >= p2 ? o4 : o2;
        }
        lastPeak = (float)peak;
        if (peak < kGatePeak) {   // noise: stay in the search, skip ahead
            idle = true;
            skipLeft_ = (uint64_t)(kIdleEvery - 1) * kAcq * (uint64_t)ratio_;
            return;
        }
        idle = false;
        if (peak > 6) cfoCoarse += off;
        updateStep();
        phase_ = 0; freq_ = 0;
        acquired = true;
    }

    template<class S> void sample(cfp y, S&& sink) {
        if (!acquired) {
            acq_.push_back(y);
            if ((int)acq_.size() >= kAcq) { acquire(); acq_.clear(); }
            return;
        }
        const cfp nco = FastSinCos::get().polar(1.f, -phase_);   // a table: std::cos and std::sin in double each sample were the largest single cost
        cfp v = y * nco;
        phase_ += freq_;
        v = rrc_.push(v);
        if (phase_ > 2 * kPi) phase_ -= 2 * kPi; else if (phase_ < -2 * kPi) phase_ += 2 * kPi;
        h_[0] = h_[1]; h_[1] = h_[2]; h_[2] = h_[3]; h_[3] = v;
        t_ -= 1.0;
        while (t_ <= 0) {
            const float d = (float)-t_;
            const float mu = 1.f - d;
            const float c0 = -mu * (mu - 1) * (mu - 2) / 6, c1 = (mu + 1) * (mu - 1) * (mu - 2) / 2, c2 = -(mu + 1) * mu * (mu - 2) / 2, c3 = (mu + 1) * mu * (mu - 1) / 6;
            const cfp a = h_[0] * c0 + h_[1] * c1 + h_[2] * c2 + h_[3] * c3;
            event(a, sink);
        }
    }

    template<class S> void event(cfp a, S&& sink) {
        if (strobePh_ == 0) { prevMid_ = mid_; mid_ = a; strobePh_ = 1; t_ += sps_ * 0.5; return; }
        strobePh_ = 0;
        const cfp prev = lastStrobe_;
        // the symbol of this strobe: OQPSK takes I of the last strobe and Q of the half symbol after it
        cfp sy = a;
        float e;
        if (mod == kXbOqpsk) {
            sy = cfp(prev.real(), mid_.imag());
            e = (prev.real() - a.real()) * mid_.real() + (prevMid_.imag() - mid_.imag()) * prev.imag();
        } else if (mod == kXbQpsk) e = (prev.real() - a.real()) * mid_.real() + (prev.imag() - a.imag()) * mid_.imag();
        else e = (prev.real() - a.real()) * mid_.real();
        const float mag2 = std::max(1e-9f, std::norm(sy));
        agc += 0.002f * (std::sqrt(mag2 / (mod == kXbBpsk ? 1 : 2)) - agc);
        e /= std::max(1e-6f, agc * agc);
        lastStrobe_ = a;
        e = std::clamp(e, -1.f, 1.f);
        const double wA = tA_, wB = tB_;
        sps_ = std::clamp(sps_ + wB * e * spsNom_, spsNom_ * 0.992, spsNom_ * 1.008);
        t_ += sps_ * 0.5 + wA * e * spsNom_ * 0.5;
        const float I = sy.real() / agc, Q = sy.imag() / agc;
        const float ec = mod == kXbBpsk ? std::clamp((I > 0 ? 1.f : -1.f) * Q, -1.f, 1.f)
                                        : std::clamp((I > 0 ? 1.f : -1.f) * Q - (Q > 0 ? 1.f : -1.f) * I, -1.f, 1.f);
        freq_ += cB_ * ec / spsNom_;
        phase_ += cA_ * ec;
        carrierLock += 0.002f * ((1.f - std::fabs(ec)) - carrierLock);
        const float ab = mod == kXbBpsk ? std::fabs(I) : 0.5f * (std::fabs(I) + std::fabs(Q));
        meanAbs_ += 0.003f * (ab - meanAbs_);
        varAbs_ += 0.003f * ((ab - meanAbs_) * (ab - meanAbs_) - varAbs_);
        if (cons.size() < 256) cons.emplace_back(I, mod == kXbBpsk ? 0.f : Q);
        else { cons[consPos] = {I, mod == kXbBpsk ? 0.f : Q}; consPos = (consPos + 1) % 256; }
        sink(I, Q);
    }
};

// 90 degree steps (t & 3) and the Q rail inverted (t & 4)
inline void xform(int t, float a, float b, float& oa, float& ob) {
    if (t & 4) b = -b;
    switch (t & 3) { case 0: oa = a; ob = b; break; case 1: oa = -b; ob = a; break; case 2: oa = -a; ob = -b; break; default: oa = b; ob = -a; break; }
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------------
struct XbandReceiver::Impl {
    const int P;
    const XbLink L;
    mutable std::mutex mu;
    std::function<void(const std::string&)> logCb;
    double rate = 0, offset = 0;
    std::atomic<bool> resetReq{false}, clearReq{false};
    XbTelemetry tel, pub;
    Demod dem;
    xband::Cadu cadu;
    xband::Store store;
    xband::Instruments inst;
    bool haveRate = false;

    // the ambiguity search
    std::vector<float> symI, symQ;
    bool locked = false;
    int hypT = 0, hypAlign = 0, frameSyms = 0;
    int sinceSearch = 0;
    uint64_t lastHits = 0;
    double lastFrameT = 0;
    // the locked path
    bool altVit = false;
    float pendingSoft = 0;
    bool havePending = false;
    int prevI = 0, prevQ = 0, nrzPrev = 0;
    hrpt::Viterbi vit;
    wxsat::Deframer def;
    std::ofstream caduOut;
    std::mutex caduMu;
    std::vector<uint8_t> frameTmp;

    double tStart = 0, lastPub = 0;
    int64_t nIn = 0;

    explicit Impl(int p) : P(std::clamp(p, 0, (int)kXbNumProfiles - 1)), L(xbLink(P)), cadu(L), inst(P, store) {
        tel.profile = pub.profile = P;
        def.onFrame = [this](const uint8_t* f) { frame(f); };
    }

    void say(const std::string& s) { if (logCb) logCb(std::string("xband ") + L.id + ": " + s); }

    int symsPerFrame() const {
        const int bits = L.caduBytes * 8;
        if (L.fec == kXbFecConv) return L.mod == kXbBpsk ? bits * 2 : bits;
        return L.mod == kXbBpsk ? bits : bits / 2;
    }

    void setup() {
        haveRate = rate >= L.minRate - 1;
        if (haveRate) { dem.configure(rate, L.symbolRate, L.mod, L.alpha); dem.setOffset(offset); }
        if (L.caduBytes > 0) def.configure(xband::kAsm, 32, L.caduBytes * 8, 3, 10, true);
        if (P == kXbDmsp) { def.configure(0b1010110011111, 13, 150, 0, 5, true); def.onFrame = [this](const uint8_t* f) { dmspFrame(f); }; }
        if (P == kXbOceansat2) for (int k = 0; k < 2; k++) { osDef[k].configure(0b0000000111111011111000010011001101ull, 34, 737760, 2, 6, false); osDef[k].setConfirm(true); osDef[k].onFrame = [this](const uint8_t* f) { oceansatFrame(f); }; }
        if (P == kXbKanopus) { kanBits.assign((size_t)kKanBits, 0); kanW = 0; kanSkip = 0; kanLocked = false; kanBad = 0; pmssFrm.clear(); pmssIn = 0; pmssSync = false; }
        frameSyms = L.caduBytes > 0 ? symsPerFrame() : 0;
        resetState();
    }
    void resetState() {
        if (haveRate) dem.resetAll();
        locked = false; symI.clear(); symQ.clear(); sinceSearch = 0;
        def.reset(); vit.reset(); havePending = false; prevI = prevQ = nrzPrev = 0; lastHits = 0;
        lastFrameT = nowSec();
        tel.state = 0; tel.frameLock = false;
    }

    // ---- bits after the (optional) convolutional decoder ----
    void outBit(int b) {
        if (L.nrzm) { const int d = b ^ nrzPrev; nrzPrev = b; b = d; }
        def.push(b);
    }
    void vitPush(float a, float b) {
        vit.push(sat8(a * 40), sat8(-b * 40));   // G2 is inverted on the link and hrpt::Viterbi expects it not inverted
        auto& o = vit.out();
        if (o.size() >= 256) { for (uint8_t x : o) outBit(x); o.clear(); }
    }

    // the symbol path under a hypothesis (t: ambiguity 0..7, align: coded pairing 0 / 1)
    void pathSymbol(float I, float Q) {
        if (L.mod == kXbOqpsk) {
            const float a0 = (hypT & 1) ? -I : I, b0 = (hypT & 2) ? -Q : Q;
            if (L.fec == kXbFecConv) {
                serialCoded(a0); serialCoded(b0);
            } else {
                int bi = a0 > 0, bq = b0 > 0;
                if (L.railDiff) { const int di = bi ^ prevI, dq = bq ^ prevQ; prevI = bi; prevQ = bq; bi = di; bq = dq; }
                outBit(bi); outBit(bq);
            }
            return;
        }
        if (L.fec == kXbFecConv) {
            if (L.mod == kXbBpsk) {
                float s = I;
                if (hypAlign == 1 && !havePending && !alignDone) { alignDone = true; return; }
                if (!havePending) { pendingSoft = s; havePending = true; return; }
                havePending = false;
                float a = pendingSoft, b = s;
                if (hypT & 1) { a = -a; b = -b; }
                if (hypT & 2) std::swap(a, b);
                vitPush(a, b);
                return;
            }
            float ta, tb; xform(hypT, I, Q, ta, tb);
            if (hypAlign == 0) { vitPush(ta, tb); return; }
            if (havePending) { vitPush(pendingSoft, ta); }
            pendingSoft = tb; havePending = true;
            return;
        }
        // uncoded
        if (L.mod == kXbBpsk) { outBit(I > 0 ? 1 : 0); return; }
        float ta, tb; xform(hypT, I, Q, ta, tb);
        int bi = ta > 0 ? 1 : 0, bq = tb > 0 ? 1 : 0;
        if (L.railDiff) { const int di = bi ^ prevI, dq = bq ^ prevQ; prevI = bi; prevQ = bq; bi = di; bq = dq; }
        outBit(bi); outBit(bq);
    }
    bool alignDone = false;
    void serialCoded(float v) {
        if (hypAlign == 1 && !alignDone) { alignDone = true; return; }
        if (!havePending) { pendingSoft = v; havePending = true; return; }
        havePending = false;
        vitPush(pendingSoft, v);
    }

    void symbol(float I, float Q) {
        if (L.caduBytes == 0) { special(I, Q); return; }
        if (locked) { pathSymbol(I, Q); return; }
        symI.push_back(I); symQ.push_back(Q);
        const size_t cap = (size_t)frameSyms * 3 + 800;
        // trim in steps of a frame: cutting one symbol off the front each symbol moved the whole buffer every symbol (on noise, where it
        // never locks, that cost more than the rest of the receiver)
        if (symI.size() > cap + (size_t)std::max(frameSyms, 1000)) { const size_t cut = symI.size() - cap; symI.erase(symI.begin(), symI.begin() + (std::ptrdiff_t)cut); symQ.erase(symQ.begin(), symQ.begin() + (std::ptrdiff_t)cut); }
        if (++sinceSearch >= std::max(frameSyms, 1000) && (int)symI.size() >= frameSyms * 2 + 800) { sinceSearch = 0; search(); }
    }

    void search() {
        static const uint32_t kAsmV = xband::kAsm;
        const int np = (int)symI.size();
        const int nT = (L.mod == kXbBpsk || L.mod == kXbOqpsk) ? 4 : 8;
        const int nAl = L.fec == kXbFecConv ? 2 : 1;
        for (int t = 0; t < nT; t++) for (int al = 0; al < nAl; al++) {
            std::vector<int> hits;
            if (L.fec == kXbFecConv) {
                // the encoded sync word, in code bits; the first 6 pairs depend on the bits before the sync
                const uint64_t pat0 = L.nrzm ? encodedAsm(false, true) : encodedAsm(false, false), pat1 = encodedAsm(true, true);
                const uint64_t mask = (1ull << 52) - 1;
                uint64_t w = 0;
                for (int i = 0; i < np; i++) {
                    float a, b;
                    if (L.mod == kXbOqpsk) {
                        // the rails in time order, the sign pattern is the ambiguity (a 90 degree step moves a rail by half a symbol)
                        const float s0 = (t & 1) ? -symI[(size_t)i] : symI[(size_t)i], s1 = (t & 2) ? -symQ[(size_t)i] : symQ[(size_t)i];
                        if (al == 0) { a = s0; b = s1; }
                        else { if (i == 0) { prevSeed_ = s1; continue; } a = prevSeed_; b = s0; prevSeed_ = s1; }
                    } else if (L.mod == kXbBpsk) {
                        if (2 * i + 1 + al >= np) break;
                        a = symI[(size_t)(2 * i + al)]; b = symI[(size_t)(2 * i + 1 + al)];
                        if (t & 1) { a = -a; b = -b; }
                        if (t & 2) std::swap(a, b);
                    } else {
                        float ta, tb; xform(t, symI[(size_t)i], symQ[(size_t)i], ta, tb);
                        if (al == 0) { a = ta; b = tb; }
                        else { if (i == 0) { w = 0; prevSeed_ = tb; continue; } a = prevSeed_; b = ta; prevSeed_ = tb; }
                    }
                    w = w << 2 | (uint64_t)(a > 0) << 1 | (uint64_t)(b > 0);
                    if (i >= 31) {
                        if (::dect2::bits::popcount((uint64_t)((w ^ pat0) & mask)) <= 10) hits.push_back(i);
                        else if (L.nrzm && ::dect2::bits::popcount((uint64_t)((w ^ pat1) & mask)) <= 10) hits.push_back(i);
                    }
                }
                const int wantFrameBits = L.caduBytes * 8;
                int at = -1;
                for (size_t x = 0; x < hits.size() && at < 0; x++) for (size_t y = x + 1; y < hits.size(); y++) if (hits[y] - hits[x] == wantFrameBits) { at = hits[y]; break; }
                if (at < 0) continue;
                if (lockOn(t, al, at + 1 - wantFrameBits)) return;
                continue;
            }
            // uncoded: look for the sync word in the bit stream of this ambiguity (both polarities: the deframer takes care of it)
            int pi = 0, pq = 0, nz = 0;
            uint32_t w = 0;
            std::vector<int> bitHits;
            const int nb = np * (L.mod == kXbBpsk ? 1 : 2);
            bitHits.reserve(64);
            const auto ob = [&](int b, int idx) {
                if (L.nrzm) { const int d = b ^ nz; nz = b; b = d; }
                w = w << 1 | (uint32_t)b;
                const int d0 = ::dect2::bits::popcount((uint32_t)(w ^ kAsmV)), d1 = ::dect2::bits::popcount((uint32_t)(~w ^ kAsmV));
                if (idx >= 31 && (d0 <= 2 || d1 <= 2)) bitHits.push_back(idx);
            };
            for (int i = 0; i < np; i++) {
                if (L.mod == kXbBpsk) { ob(symI[(size_t)i] > 0 ? 1 : 0, i); continue; }
                float ta, tb;
                if (L.mod == kXbOqpsk) { ta = (t & 1) ? -symI[(size_t)i] : symI[(size_t)i]; tb = (t & 2) ? -symQ[(size_t)i] : symQ[(size_t)i]; }
                else xform(t, symI[(size_t)i], symQ[(size_t)i], ta, tb);
                int bi = ta > 0, bq = tb > 0;
                if (L.railDiff) { const int di = bi ^ pi, dq = bq ^ pq; pi = bi; pq = bq; bi = di; bq = dq; }
                ob(bi, 2 * i); ob(bq, 2 * i + 1);
            }
            int at = -1;
            const int fb = L.caduBytes * 8;
            for (size_t x = 0; x < bitHits.size() && at < 0; x++) for (size_t y = x + 1; y < bitHits.size(); y++) if (bitHits[y] - bitHits[x] == fb) { at = bitHits[y]; break; }
            if (at < 0) continue;
            (void)nb;
            if (lockOn(t, 0, (at + 1 - fb) / (L.mod == kXbBpsk ? 1 : 2))) return;
        }
        if (!locked && symI.size() > (size_t)frameSyms * 3) tel.state = std::max(tel.state, 1);
    }
    float prevSeed_ = 0;

    // A hit can be a false one (a wrong pairing of a rate 1/2 BPSK stream looks like the sync word for one frame): replay the buffer and keep the hypothesis only when a frame decoded.
    bool lockOn(int t, int al, int from) {
        hypT = t; hypAlign = al;
        locked = true;
        vit.reset(); def.reset(); havePending = false; alignDone = false; prevI = prevQ = nrzPrev = 0;
        const uint64_t okBefore = tel.blocksOk, hitsBefore = tel.asmHits, badBefore = tel.blocksBad;
        const int np = (int)symI.size();
        int start = std::max(0, from - 200);
        if (L.mod == kXbBpsk && L.fec == kXbFecConv) { start = std::max(0, (from - 200) * 2) + al; alignDone = true; }
        for (int i = start; i < np; i++) pathSymbol(symI[(size_t)i], symQ[(size_t)i]);
        const bool good = tel.blocksOk > okBefore || (L.rsI == 0 && tel.asmHits >= hitsBefore + 2);
        if (!good) { locked = false; tel.blocksBad = badBefore; return false; }
        symI.clear(); symQ.clear();
        char b[120];
        snprintf(b, sizeof b, "sync word found, ambiguity %d, pairing %d", t, al);
        tel.phase = b;
        say(b);
        lastFrameT = nowSec();
        return true;
    }

    // ---- frames ----
    void frame(const uint8_t* f) {
        frameTmp.assign(f, f + L.caduBytes);
        tel.asmHits++;
        int e = cadu.decode(frameTmp.data());
        if (e < 0 && P == kXbProbaS) {
            // Proba-1 and Proba-2 are not randomised
            frameTmp.assign(f, f + L.caduBytes);
            XbLink nr = L; nr.derand = false;
            e = xband::Cadu(nr).decode(frameTmp.data());
        }
        if (e < 0) { tel.blocksBad++; tel.state = std::max(tel.state, 2); return; }
        tel.rsCorrected += (uint64_t)e;
        accept();
    }
    void accept() {
        tel.blocksOk++; tel.state = 3; tel.frameLock = true; lastFrameT = nowSec();
        xband::Header h;
        xband::parseHeader(L, frameTmp.data(), h);
        tel.scid = h.scid; tel.vcid = h.vcid;
        tel.vcidFrames[h.vcid & 63]++;
        {
            std::lock_guard<std::mutex> g(store.mu);
            inst.frame(frameTmp.data());
        }
        std::lock_guard<std::mutex> g2(caduMu);
        if (caduOut.is_open()) { caduOut.write((const char*)frameTmp.data(), L.caduBytes); tel.caduWritten++; }
    }

    // Oceansat-2: DQPSK, one bit a symbol, tried with the two rail orders
    wxsat::Deframer osDef[2];
    int osPrev[2] = {0, 0};
    void oceansatFrame(const uint8_t* f) {
        tel.blocksOk++; tel.state = 3; lastFrameT = nowSec(); tel.asmHits++;
        std::vector<uint8_t> b(f, f + 92220);
        std::lock_guard<std::mutex> g(store.mu);
        inst.oceansatFrame(b.data());
    }
    // Kanopus-V: QPSK differential, 3072-byte outer frames, PMSS frames inside
    static constexpr int kKanBits = 3072 * 8;
    std::vector<uint8_t> kanBits;
    int kanW = 0, kanSkip = 0, kanBad = 0;
    bool kanLocked = false;
    int qdPrevX = 0, qdPrevY = 0; bool qdHave = false;
    std::vector<uint8_t> pmssFrm; int pmssIn = 0, pmssHist = 0; uint8_t pmssSr[16] = {}; bool pmssSync = false;
    inline int kanBit(int idx) const { return kanBits[(size_t)((kanW + idx) % kKanBits)]; }
    void kanPush(int b) {
        kanBits[(size_t)kanW] = (uint8_t)b; kanW = (kanW + 1) % kKanBits;
        if (kanSkip > 0) { kanSkip--; return; }
        static const uint8_t sy[4] = {0b00000100, 0b10110011, 0b11100011, 0b01110101};
        int diff = 0;
        for (int f = 0; f < 2; f++) for (int s2 = 0; s2 < 4; s2++) { uint8_t v = 0; for (int k = 0; k < 8; k++) v = (uint8_t)(v << 1 | kanBit((s2 * 384 + k) * 8 + f)); diff += ::dect2::bits::popcount((uint32_t)(v ^ sy[s2])); }
        if (!kanLocked ? diff == 0 : diff < 10) {
            kanLocked = true; kanBad = 0;
            kanWrite();
            kanSkip = kKanBits - 1;
        } else if (kanLocked && ++kanBad > 10) kanLocked = false;
    }
    void kanWrite() {
        static const uint8_t der[48] = {0x55,0xE2,0xA4,0x6A,0x30,0xA7,0xDA,0x16,0x24,0x86,0x39,0x07,0x0D,0x1D,0xEC,0x64,0xF0,0x3D,0xD7,0x66,0x98,0x08,0x15,0x94,0xA0,0xBA,0x5B,0x22,0x3E,0x67,0x40,0x1B,0x54,0x3A,0xB7,0x2B,0x9E,0xB0,0x4B,0xD3,0xB6,0xF3,0x8D,0xF1,0xE5,0xC4,0x27,0x36};
        uint8_t dei[8][384] = {};
        for (int i = 0; i < kKanBits; i++) dei[i % 8][i / 64] = (uint8_t)(dei[i % 8][i / 64] << 1 | kanBit(i));
        for (int f = 0; f < 8; f++) for (int i = 0; i < 8; i++) for (int k = 0; k < 48; k++) dei[f][i * 48 + k] ^= der[k];
        uint8_t fin[384 * 8 - 16];
        int y = 0;
        for (int i = 0; i < 8; i++) for (int k = 0; k < 48; k++) for (int f : {1, 0, 3, 2, 5, 4, 7, 6}) { if (f < 2 && k == 0) continue; if (y < (int)sizeof fin) fin[y++] = dei[f][i * 48 + k]; }
        for (int b = 0; b < y; b++) pmssByte(fin[b]);
    }
    void pmssByte(uint8_t b) {
        std::memmove(pmssSr, pmssSr + 1, 15); pmssSr[15] = b;
        if (pmssIn > 0) {
            pmssFrm.push_back(b);
            if (--pmssIn == 0) {
                tel.blocksOk++; tel.state = 3; lastFrameT = nowSec();
                std::lock_guard<std::mutex> g(store.mu);
                inst.kanopusFrame(pmssFrm.data());
            }
            return;
        }
        if (pmssSkip > 0) { pmssSkip--; return; }
        static const uint8_t sw[16] = {0xf5,0x0a,0xf5,0x0a,0xf5,0x0a,0xf5,0x0a,0xf5,0x0a,0xf5,0x0a,0xf5,0x0a,0xf5,0x0a};
        int e = 0;
        for (int i = 0; i < 16; i++) e += ::dect2::bits::popcount((uint32_t)((unsigned)(pmssSr[i] ^ sw[i])));
        if (pmssSync ? e < 8 : e == 0) {
            pmssIn = 1960 - 16; pmssSkip = 15;
            pmssFrm.assign(pmssSr, pmssSr + 16);
            if (e == 0) pmssSync = true;
        } else pmssSync = false;
    }
    int pmssSkip = 0;

    void special(float I, float Q) {
        if (P == kXbDmsp) { def.push(I > 0 ? 1 : 0); tel.frameLock = def.locked(); }
        else if (P == kXbOceansat2) {
            const bool ia = I > 0, ib = Q > 0;
            const int v = ia ? (ib ? 0 : 3) : (ib ? 1 : 2);   // the quadrant, as SatDump's dqpsk_demod
            const int a[2] = {v, ((v & 1) << 1) | (v >> 1)};
            for (int k = 0; k < 2; k++) { const int d = (a[k] - osPrev[k]) & 3; osPrev[k] = a[k]; osDef[k].push((d >> 1) & 1); }
        } else if (P == kXbKanopus) {
            const int xi = I >= 0 ? 1 : 0, yi = Q >= 0 ? 1 : 0;
            if (!qdHave) { qdHave = true; qdPrevX = xi; qdPrevY = yi; return; }
            int xo, yo;
            if ((xi ^ yi) == 1) { xo = qdPrevY ^ yi; yo = qdPrevX ^ xi; } else { xo = qdPrevX ^ xi; yo = qdPrevY ^ yi; }
            qdPrevX = xi; qdPrevY = yi;
            kanPush(xo); kanPush(yo ^ 1);
            tel.frameLock = kanLocked;
        }
    }
    void dmspFrame(const uint8_t* f) {
        tel.blocksOk++; tel.state = 3; lastFrameT = nowSec(); tel.asmHits++;
        std::lock_guard<std::mutex> g(store.mu);
        inst.rawFrame(f);
    }

    // ---- feed ----
    void feed(const cf32* x, size_t n) {
        if (resetReq.exchange(false)) resetState();
        if (clearReq.exchange(false)) { std::lock_guard<std::mutex> g(store.mu); store.clear(); }
        if (!haveRate) { tel.rateTooLow = true; maybePublish(n); return; }
        double e = 0;
        const size_t st = std::max<size_t>(1, n / 64);
        for (size_t i = 0; i < n; i += st) e += (double)x[i].real() * x[i].real() + (double)x[i].imag() * x[i].imag();
        tel.levelDb = (float)(10 * std::log10(std::max(1e-12, e / std::max<size_t>(1, n / st))));
        dem.feed(x, n, [this](float I, float Q) { symbol(I, Q); });
        nIn += (int64_t)n;
        if (L.fec == kXbFecConv) { /* the Viterbi output stays in the buffer until 256 bits, see vitPush */ }
        const double now = nowSec();
        if (dem.acquired) tel.state = std::max(tel.state, dem.carrierLock > 0.5f ? 1 : 0);
        if (L.caduBytes == 0 && !def.locked() && P == kXbDmsp && tel.state == 3 && now - lastFrameT > 1) tel.state = 1;
        if (locked && !def.locked() && now - lastFrameT > std::max(1.0, 0.5 * (double)frameSyms / L.symbolRate * 4)) {
            locked = false; symI.clear(); symQ.clear(); tel.state = dem.carrierLock > 0.5f ? 1 : 0; tel.frameLock = false;
        }
        if (now - lastFrameT > 8 && tel.state < 2) { dem.reacquire(); lastFrameT = now; }
        maybePublish(n);
    }

    void pushCaduFrame(const uint8_t* f) {
        if (resetReq.exchange(false)) resetState();
        if (clearReq.exchange(false)) { std::lock_guard<std::mutex> g(store.mu); store.clear(); }
        tel.asmHits++;
        frameTmp.assign(f, f + L.caduBytes);
        int e = cadu.decode(frameTmp.data());
        if (e < 0 && P == kXbProbaS) { frameTmp.assign(f, f + L.caduBytes); XbLink nr = L; nr.derand = false; e = xband::Cadu(nr).decode(frameTmp.data()); }
        if (e < 0) { tel.blocksBad++; tel.state = 2; maybePublish(0); return; }
        tel.rsCorrected += (uint64_t)e;
        accept();
        maybePublish(0);
    }

    // every 0.25 s, and at least every second of signal: a recording read faster than that (DMSP: 3 s in 0.24 s once the receiver got
    // cheaper) kept its first report to the end otherwise
    int64_t lastPubN = 0;
    void maybePublish(size_t) {
        const double now = nowSec();
        if (now - lastPub < 0.25 && !(rate > 0 && (double)(nIn - lastPubN) >= rate)) return;
        lastPub = now; lastPubN = nIn;
        tel.packets = inst.packets();
        tel.lines = inst.lines();
        tel.satName = inst.satName();
        tel.scid = inst.lastScid() >= 0 ? inst.lastScid() : tel.scid;
        tel.cfoHz = dem.currentCfo();
        tel.snrDb = dem.snrDb(); tel.carrierLock = dem.carrierLock; tel.acqPeak = dem.lastPeak; tel.idleSkipped = dem.skipped;
        tel.symbolRate = L.symbolRate; tel.inputRate = rate;
        tel.dataValid = tel.blocksOk > 0;
        tel.frameLock = tel.frameLock && def.locked();
        tel.constellation = dem.cons;
        tel.seq++;
        {
            std::lock_guard<std::mutex> g(store.mu);
            tel.channels = store.previews(1600);
        }
        std::lock_guard<std::mutex> g(mu);
        pub = tel;
    }

    bool savePlanes(const std::string& folder, std::string* err) const;

    static uint64_t encodedAsm(bool nrz1, bool nrz) {
        hrpt::CcsdsEncoder e;
        uint64_t v = 0;
        int nz = nrz1 ? 1 : 0;
        for (int i = 31; i >= 0; i--) {
            int b = (int)(xband::kAsm >> i & 1);
            if (nrz) { nz ^= b; b = nz; }
            uint8_t a, bb;
            e.push(b, a, bb);
            v = v << 2 | (uint64_t)a << 1 | (uint64_t)bb;
        }
        return v;
    }
};

XbandReceiver::XbandReceiver(int p) : p_(std::make_unique<Impl>(p)) {}
XbandReceiver::~XbandReceiver() = default;
void XbandReceiver::configure(double r) { p_->rate = r; p_->setup(); }
void XbandReceiver::setSignalOffset(double hz) { p_->offset = hz; if (p_->haveRate) p_->dem.setOffset(hz); }
bool XbandReceiver::ready() const { return p_->rate > 0; }
void XbandReceiver::reset() { p_->resetReq = true; }
void XbandReceiver::clearImage() { p_->clearReq = true; }
void XbandReceiver::feed(const cf32* x, size_t n) { p_->feed(x, n); }
bool XbandReceiver::telemetry(XbTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> g(p_->mu);
    if (p_->pub.seq == lastSeq) return false;
    out = p_->pub;
    return true;
}
void XbandReceiver::clearResults() { p_->clearReq = true; }
void XbandReceiver::setLogCallback(std::function<void(const std::string&)> cb) { p_->logCb = std::move(cb); }
int XbandReceiver::profile() const { return p_->P; }
void XbandReceiver::pushCadu(const uint8_t* cadu) { p_->pushCaduFrame(cadu); }
void XbandReceiver::finish() { { std::lock_guard<std::mutex> g(p_->store.mu); p_->inst.finish(); } p_->lastPub = 0; p_->maybePublish(0); }
bool XbandReceiver::setCaduOutput(const std::string& path) {
    std::lock_guard<std::mutex> g(p_->caduMu);
    if (p_->caduOut.is_open()) p_->caduOut.close();
    if (path.empty()) return true;
    p_->caduOut.open(path, std::ios::binary | std::ios::app);
    return p_->caduOut.is_open();
}
std::vector<uint8_t> XbandReceiver::loopGif(const std::vector<XbProductImage>& frames, int delayCs) const {
    std::vector<xband::XbProduct> v;
    for (const auto& f : frames) { xband::XbProduct p; p.width = f.width; p.height = f.height; p.rgba = f.rgba; v.push_back(std::move(p)); }
    return xband::makeLoopGif(v, delayCs);
}
std::vector<XbProductImage> XbandReceiver::products(int maxWidth) const {
    std::vector<XbProductImage> out;
    std::lock_guard<std::mutex> g(p_->store.mu);
    for (auto& v : xband::makeProducts(p_->store, p_->P, p_->inst.avhrrCal(), maxWidth)) out.push_back({v.name, v.note, v.width, v.height, std::move(v.rgba)});
    return out;
}
bool XbandReceiver::readPlane(int c, int& w, int& h, std::vector<uint16_t>& px) const {
    std::lock_guard<std::mutex> g(p_->store.mu);
    if (c < 0 || c >= p_->store.count()) return false;
    const auto& pl = p_->store.plane(c);
    w = pl.width; h = pl.lines; px = pl.px;
    return true;
}
std::vector<XbProductImage> XbandReceiver::channelImages() const {
    std::vector<XbProductImage> out;
    std::lock_guard<std::mutex> g(p_->store.mu);
    for (int i = 0; i < p_->store.count(); i++) {
        const xband::Plane& pl = p_->store.plane(i);
        if (pl.lines == 0) continue;
        XbProductImage m; m.name = pl.name; m.width = pl.width; m.height = pl.lines;
        // stretch on the data range
        uint32_t lo = 65535, hi = 0;
        for (size_t k = 0; k < pl.px.size(); k += 11) if (pl.px[k]) { lo = std::min<uint32_t>(lo, pl.px[k]); hi = std::max<uint32_t>(hi, pl.px[k]); }
        if (hi <= lo) { lo = 0; hi = 65535; }
        m.rgba.resize((size_t)m.width * m.height);
        for (size_t k = 0; k < m.rgba.size(); k++) {
            const uint32_t v = (uint32_t)std::clamp<int64_t>((int64_t)((pl.px[k] - (int64_t)lo)) * 255 / std::max<int64_t>(1, (int64_t)hi - lo), 0, 255);
            m.rgba[k] = 0xFF000000u | v << 16 | v << 8 | v;
        }
        out.push_back(std::move(m));
    }
    return out;
}

// ---- recordings ---------------------------------------------------------------------------------------------------------------
struct XbFileRunner::Impl {
    std::thread th;
    std::atomic<bool> stop{false}, running{false};
    std::atomic<float> progress{0};
    std::mutex mu;
    std::string status;
    void say(std::string s) { std::lock_guard<std::mutex> g(mu); status = std::move(s); }
};
XbFileRunner::XbFileRunner() : p_(std::make_unique<Impl>()) {}
XbFileRunner::~XbFileRunner() { stop(); }
void XbFileRunner::stop() { p_->stop = true; if (p_->th.joinable()) p_->th.join(); p_->running = false; }
bool XbFileRunner::running() const { return p_->running; }
float XbFileRunner::progress() const { return p_->progress; }
std::string XbFileRunner::status() const { std::lock_guard<std::mutex> g(p_->mu); return p_->status; }

namespace {
enum Sample { kCu8, kCs8, kCs16, kCf32 };
bool sampleFmt(const std::string& ext, Sample& s) {
    if (ext == "cu8" || ext == "u8") { s = kCu8; return true; }
    if (ext == "cs8" || ext == "s8") { s = kCs8; return true; }
    if (ext == "cs16" || ext == "s16" || ext == "sc16") { s = kCs16; return true; }
    if (ext == "cf32" || ext == "fc32" || ext == "c64" || ext == "bin" || ext == "raw") { s = kCf32; return true; }
    return false;
}
std::string lowerExt(const std::string& p) {
    const size_t d = p.rfind('.');
    if (d == std::string::npos) return "";
    std::string e = p.substr(d + 1);
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}
// A SigMF metadata file: the two fields we need, by plain text search
bool readSigmf(const std::string& meta, Sample& s, double& rate) {
    std::ifstream f(meta);
    if (!f) return false;
    std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const auto field = [&](const char* k) { const size_t at = t.find(k); if (at == std::string::npos) return std::string(); size_t c = t.find(':', at + strlen(k)); if (c == std::string::npos) return std::string(); c++; while (c < t.size() && (t[c] == ' ' || t[c] == '"')) c++; size_t e = c; while (e < t.size() && t[e] != '"' && t[e] != ',' && t[e] != '}' && t[e] != '\n') e++; return t.substr(c, e - c); };
    const std::string dt = field("core:datatype");
    if (dt.find("cf32") == 0) s = kCf32; else if (dt.find("ci16") == 0 || dt.find("cu16") == 0) s = kCs16; else if (dt.find("ci8") == 0) s = kCs8; else if (dt.find("cu8") == 0) s = kCu8; else return false;
    const std::string sr = field("core:sample_rate");
    rate = sr.empty() ? 0 : std::atof(sr.c_str());
    return true;
}
}

bool XbFileRunner::start(XbandReceiver& rx, const XbFileJob& j, std::string* err) {
    stop();
    p_->stop = false;
    const auto fail = [&](const char* m) { if (err) *err = m; return false; };
    std::string path = j.path;
    std::string ext = lowerExt(path);
    Sample smp = kCs16;
    double rate = j.sampleRate;
    bool isCadu = ext == "cadu";
    int64_t dataOff = 0;
    bool wav = ext == "wav";
    const auto dot = path.rfind('.');
    if (ext == "sigmf-meta" || ext == "sigmf-data") {
        const std::string base = path.substr(0, dot);
        if (!readSigmf(base + ".sigmf-meta", smp, rate)) return fail("cannot read the SigMF metadata");
        path = base + ".sigmf-data";
        if (j.sampleRate > 0) rate = j.sampleRate;
    } else if (!isCadu && !wav) {
        if (!j.format.empty()) { if (!sampleFmt(j.format, smp)) return fail("unknown sample format"); }
        else if (!sampleFmt(ext, smp)) return fail("unknown file type: use .cadu, .wav, .sigmf or raw .cs8/.cu8/.cs16/.cf32 with a sample rate");
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail("cannot open the file");
    f.seekg(0, std::ios::end);
    const int64_t size = (int64_t)f.tellg();
    f.seekg(0);
    if (wav) {
        char h[44];
        f.read(h, 44);
        const int bits = (uint8_t)h[34] | ((uint8_t)h[35] << 8);
        const int fmt = (uint8_t)h[20] | ((uint8_t)h[21] << 8);
        rate = (double)((uint8_t)h[24] | ((uint8_t)h[25] << 8) | ((uint8_t)h[26] << 16) | ((uint32_t)(uint8_t)h[27] << 24));
        smp = bits == 8 ? kCu8 : (fmt == 3 ? kCf32 : kCs16);
        dataOff = 44;
    }
    if (!isCadu && rate <= 0) return fail("this file needs a sample rate");
    p_->running = true;
    p_->progress = 0;
    XbandReceiver* rp = &rx;
    const double r2 = rate, off2 = j.offsetHz;
    const int bytesS = isCadu ? 1 : smp == kCf32 ? 8 : smp == kCs16 ? 4 : 2;
    p_->th = std::thread([this, f = std::move(f), rp, isCadu, smp, r2, off2, size, dataOff, bytesS]() mutable {
        (void)dataOff;
        const XbLink& L = xbLink(rp->profile());
        int64_t done = 0;
        if (isCadu) {
            if (L.caduBytes <= 0) { p_->say("this link has no CADUs"); p_->running = false; return; }
            std::vector<uint8_t> b((size_t)L.caduBytes);
            while (!p_->stop && f.read((char*)b.data(), (std::streamsize)b.size())) {
                rp->pushCadu(b.data());
                done += L.caduBytes;
                p_->progress = (float)done / std::max<int64_t>(1, size);
            }
        } else {
            rp->configure(r2);
            if (off2 != 0) rp->setSignalOffset(off2);
            std::vector<uint8_t> raw((size_t)bytesS << 18);
            std::vector<cf32> buf((size_t)1 << 18);
            while (!p_->stop && f.read((char*)raw.data(), (std::streamsize)raw.size())) {
                for (size_t i = 0; i < buf.size(); i++) {
                    float a, b;
                    const uint8_t* s = &raw[i * bytesS];
                    if (smp == kCu8) { a = (s[0] - 127.5f) / 127.5f; b = (s[1] - 127.5f) / 127.5f; }
                    else if (smp == kCs8) { a = (int8_t)s[0] / 127.f; b = (int8_t)s[1] / 127.f; }
                    else if (smp == kCs16) { a = (int16_t)(s[0] | s[1] << 8) / 32768.f; b = (int16_t)(s[2] | s[3] << 8) / 32768.f; }
                    else { float fv[2]; std::memcpy(fv, s, 8); a = fv[0]; b = fv[1]; }
                    buf[i] = cf32(a, b);
                }
                rp->feed(buf.data(), buf.size());
                done += (int64_t)raw.size();
                p_->progress = (float)done / std::max<int64_t>(1, size);
            }
        }
        rp->finish();
        p_->progress = 1;
        p_->say(p_->stop ? "stopped" : "finished");
        p_->running = false;
    });
    return true;
}

ModeTuning xbandTuning(int p) {
    const XbLink& i = xbLink(p);
    ModeTuning t;
    t.stdMode = i.family + 2; t.id = i.id; t.name = i.name;
    t.minMhz = 1; t.maxMhz = 12000; t.defMhz = i.defMhz;
    t.sampleRate = i.sampleRate; t.basebandHz = 0;
    t.bandwidthMhz = i.symbolRate / 1e6 * 1.4;
    t.minSampleRate = i.minRate;
    return t;
}

} // namespace dect2
