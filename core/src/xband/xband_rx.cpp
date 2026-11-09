// See xband_rx.h. Link parameters from SatDump's pipelines (GPL-3.0, Copyright Aang23 and contributors); code written for OnAir.
#include "dect2/xband/xband_rx.h"
#include "dect2/xband/xband_proto.h"
#include "dect2/common/mathconst.h"
#include "dect2/dsp/dsp_compat.h"
#include "dect2/dsp/rrc.h"
#include "dect2/hrpt/hrpt_proto.h"
#include "dect2/wxsat/wxsat_proto.h"
#include <algorithm>
#include <atomic>
#include <chrono>
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

// Decimating FIR, complex data, doubled ring
struct FirDec {
    std::vector<float> taps, re, im;
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
        re.assign((size_t)n * 2, 0); im.assign((size_t)n * 2, 0);
        pos = 0; cnt = 0;
    }
    void clear() { std::fill(re.begin(), re.end(), 0.f); std::fill(im.begin(), im.end(), 0.f); pos = 0; cnt = 0; }
    bool push(cfp x, cfp& out) {
        pos = (pos == 0 ? n : pos) - 1;
        re[(size_t)pos] = re[(size_t)pos + n] = x.real();
        im[(size_t)pos] = im[(size_t)pos + n] = x.imag();
        if (++cnt < dec) return false;
        cnt = 0;
        float a = 0, b = 0;
        const float* r = &re[(size_t)pos], *i2 = &im[(size_t)pos];
        for (int k = 0; k < n; k++) { a += taps[(size_t)k] * r[k]; b += taps[(size_t)k] * i2[k]; }
        out = {a, b};
        return true;
    }
};

struct Rrc {
    std::vector<float> h, re, im;
    int n = 0, pos = 0;
    void design(double sps, double alpha) {
        const int half = std::max(4, (int)std::ceil(4 * sps));
        n = 2 * half + 1;
        h.resize((size_t)n);
        double e = 0;
        for (int i = 0; i < n; i++) { h[(size_t)i] = (float)dsp::rrc((i - half) / sps, alpha); e += h[(size_t)i] * h[(size_t)i]; }
        for (auto& v : h) v /= (float)std::sqrt(e);
        re.assign((size_t)n * 2, 0); im.assign((size_t)n * 2, 0);
        pos = 0;
    }
    void clear() { std::fill(re.begin(), re.end(), 0.f); std::fill(im.begin(), im.end(), 0.f); pos = 0; }
    cfp push(cfp x) {
        pos = (pos == 0 ? n : pos) - 1;
        re[(size_t)pos] = re[(size_t)pos + n] = x.real();
        im[(size_t)pos] = im[(size_t)pos + n] = x.imag();
        float a = 0, b = 0;
        const float* r = &re[(size_t)pos], *i2 = &im[(size_t)pos];
        for (int k = 0; k < n; k++) { a += h[(size_t)k] * r[k]; b += h[(size_t)k] * i2[k]; }
        return {a, b};
    }
};

class Demod {
public:
    double inRate = 0, sym = 0, offsetHz = 0, decRate = 0;
    XbMod mod = kXbQpsk;
    bool acquired = false;
    double cfoCoarse = 0;
    float agc = 1, carrierLock = 0, snrDb = 0;
    std::vector<std::pair<float, float>> cons;
    size_t consPos = 0;

    void configure(double rate, double symbolRate, XbMod m, double alpha) {
        inRate = rate; sym = symbolRate; mod = m;
        int ratio = (int)std::floor(rate / (4.0 * sym));
        if (ratio < 2) ratio = 1;
        ratio_ = ratio;
        decRate = rate / ratio;
        if (ratio > 1) fir_.design(ratio, 0.5 / ratio * 0.95);
        spsNom_ = decRate / sym;
        rrc_.design(spsNom_, alpha);
        loopGains(m == kXbBpsk ? 0.008 : 0.005, 0.707, cA_, cB_);
        tBl_ = 0.006;
        resetAll();
    }
    void setOffset(double hz) { offsetHz = hz; updateStep(); }
    void resetAll() {
        fir_.clear(); rrc_.clear();
        rot_ = 1; for (auto& h : h_) h = 0;
        acquired = false; acq_.clear(); acq_.reserve(kAcq); cfoCoarse = 0; updateStep();
        phase_ = 0; freq_ = 0; t_ = spsNom_ / 2; strobePh_ = 0; lastStrobe_ = 0; mid_ = 0; prevMid_ = 0; sps_ = spsNom_;
        agc = 0.1f; carrierLock = 0; snrDb = 0; meanAbs_ = 0; varAbs_ = 0;
        cons.clear(); consPos = 0;
    }
    void reacquire() { acquired = false; acq_.clear(); }
    double currentCfo() const { return cfoCoarse + freq_ * decRate / (2 * kPi); }

    template<class S> void feed(const cf32* x, size_t n, S&& sink) {
        for (size_t i = 0; i < n; i++) {
            rot_ *= step_;
            if (((++renorm_) & 4095) == 0) rot_ /= std::abs(rot_);
            cfp v = cfp(x[i].real(), x[i].imag()) * rot_;
            cfp y;
            if (ratio_ == 1) y = v; else if (!fir_.push(v, y)) continue;
            sample(y, sink);
        }
    }

private:
    static constexpr int kAcq = 1 << 15;
    std::vector<cfp> acq_;
    FirDec fir_;
    Rrc rrc_;
    int ratio_ = 1;
    double spsNom_ = 2, sps_ = 2, cA_ = 0.01, cB_ = 1e-4, tBl_ = 0.005;
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
        for (int i = 0; i < N; i++) {
            cfp v = src[(size_t)i];
            cfp w = mult == 4 ? (v * v) * (v * v) : v * v;
            w /= std::max(1e-7f, std::abs(w));
            const float win = 0.5f - 0.5f * (float)std::cos(2 * kPi * i / N);
            re[(size_t)i] = w.real() * win; im[(size_t)i] = w.imag() * win;
        }
        fftSplit(re.data(), im.data(), log2n, false);
        for (int i = 0; i < N; i++) pow[(size_t)i] = re[(size_t)i] * re[(size_t)i] + im[(size_t)i] * im[(size_t)i];
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
            if (getenv("XB_DBG")) printf("acq p4 %.1f off %.0f  p2 %.1f off %.0f\n", p4, o4, p2, o2);
            peak = p4 >= p2 ? p4 : p2; off = p4 >= p2 ? o4 : o2;
        }
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
        const cfp nco((float)std::cos(phase_), (float)-std::sin(phase_));
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
        double wA, wB;
        loopGains(tBl_, 0.9, wA, wB);
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
        snrDb = 10.f * (float)std::log10(std::max(1e-3f, meanAbs_ * meanAbs_ / std::max(1e-6f, 2 * varAbs_)));
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
        if (symI.size() > cap) { const size_t cut = symI.size() - cap; symI.erase(symI.begin(), symI.begin() + (std::ptrdiff_t)cut); symQ.erase(symQ.begin(), symQ.begin() + (std::ptrdiff_t)cut); }
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
                    if (i >= 31 && getenv("XB_DBG2") && i % 1000 == 0) printf("%llx %llx\n", (unsigned long long)w, (unsigned long long)pat0);
                    if (i >= 31) {
                        if (__builtin_popcountll((w ^ pat0) & mask) <= 6) hits.push_back(i);
                        else if (L.nrzm && __builtin_popcountll((w ^ pat1) & mask) <= 6) hits.push_back(i);
                    }
                }
                const int wantFrameBits = L.caduBytes * 8;
                if (getenv("XB_DBG")) printf("t %d al %d: %zu hits, np %d\n", t, al, hits.size(), np);
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
                const int d0 = __builtin_popcount(w ^ kAsmV), d1 = __builtin_popcount(~w ^ kAsmV);
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
        const int start = std::max(0, from - 200);
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

    void special(float, float) {}

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

    void maybePublish(size_t) {
        const double now = nowSec();
        if (now - lastPub < 0.25) return;
        lastPub = now;
        tel.packets = inst.packets();
        tel.lines = inst.lines();
        tel.satName = inst.satName();
        tel.scid = inst.lastScid() >= 0 ? inst.lastScid() : tel.scid;
        tel.cfoHz = dem.currentCfo();
        tel.snrDb = dem.snrDb; tel.carrierLock = dem.carrierLock;
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
bool XbandReceiver::savePlanes(const std::string&, std::string* err) const { if (err) *err = "not built yet"; return false; }

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
