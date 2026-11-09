// Demodulator of the SatDump tier 3 links: a copy of the one in wxsat_rx.cpp (kept separate: that file is changing on other branches) with OQPSK and FSK added.
// Written for OnAir; link parameters from SatDump (GPL-3.0, Copyright Aang23 and contributors).
#pragma once
#include "dect2/common/mathconst.h"
#include "dect2/dsp/dsp_compat.h"
#include "dect2/dsp/rrc.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace dect2::sd3 {
using math::kPi;
using cfp = std::complex<float>;

// Decimating FIR, complex data, doubled ring
struct FirDec {
    std::vector<float> taps, re, im;
    int pos = 0, n = 0, dec = 1, cnt = 0;
    void design(int ratio, double cutoffNorm /* cycles a sample */) {
        dec = std::max(1, ratio);
        n = std::max(3, 12 * dec + 1);
        taps.assign((size_t)n, 0);
        double sum = 0;
        for (int i = 0; i < n; i++) {
            const double t = i - (n - 1) / 2.0;
            const double s = t == 0 ? 2 * cutoffNorm : std::sin(2 * kPi * cutoffNorm * t) / (kPi * t);
            const double w = 0.54 - 0.46 * std::cos(2 * kPi * i / (n - 1));
            taps[(size_t)i] = (float)(s * w);
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

// Symmetric matched filter
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
    cfp push(cfp x, bool complexIn) {
        pos = (pos == 0 ? n : pos) - 1;
        re[(size_t)pos] = re[(size_t)pos + n] = x.real();
        float a = 0;
        const float* r = &re[(size_t)pos];
        for (int k = 0; k < n; k++) a += h[(size_t)k] * r[k];
        if (!complexIn) return {a, 0};
        im[(size_t)pos] = im[(size_t)pos + n] = x.imag();
        float b = 0;
        const float* i2 = &im[(size_t)pos];
        for (int k = 0; k < n; k++) b += h[(size_t)k] * i2[k];
        return {a, b};
    }
};

enum Mod { Pm, Bpsk, Qpsk, Oqpsk, Fsk };

inline void loopGains(double bl, double zeta, double& a, double& b) {
    const double w = bl / (zeta + 1.0 / (4 * zeta));
    const double d = 1 + 2 * zeta * w + w * w;
    a = 4 * zeta * w / d; b = 4 * w * w / d;
}

class Demod {
public:
    double inRate = 0, sym = 0, offsetHz = 0, decRate = 0;
    Mod mod = Bpsk;
    bool acquired = false, acqPending = false;
    double cfoCoarse = 0;
    float agc = 1, carrierLock = 0, snrDb = 0;
    std::vector<std::pair<float, float>> cons;
    size_t consPos = 0;
    double acqPeak = 0;

    void configure(double rate, double symbolRate, Mod m, double alpha) {
        inRate = rate; sym = symbolRate; mod = m; alpha_ = alpha;
        int ratio = (int)std::floor(rate / (4.0 * sym));
        if (ratio < 2) ratio = 1;
        ratio_ = ratio;
        decRate = rate / ratio;
        if (ratio > 1) fir_.design(ratio, 0.5 / ratio * 0.95);
        spsNom_ = decRate / sym;
        rrc_.design(spsNom_, alpha);
        // loop bandwidths per symbol
        loopGains(m == Pm ? 0.004 * (spsNom_ / 4.0) : m == Bpsk ? 0.008 : 0.005, 0.707, cA_, cB_);
        tBl_ = m == Fsk ? 0.02 : 0.006;
        resetAll();
    }
    void setOffset(double hz) { offsetHz = hz; updateStep(); }
    void resetAll() {
        fir_.clear(); rrc_.clear();
        rot_ = 1; for (auto& h : h_) h = 0;
        acquired = (mod == Fsk); acq_.clear(); acq_.reserve(kAcq); cfoCoarse = 0; updateStep();
        prevFsk_ = {1, 0}; fskDc_ = 0; fskAvg_ = 0; prevI_ = 0; oqHave_ = false;
        phase_ = 0; freq_ = 0; t_ = spsNom_ / 2; strobePh_ = 0; lastStrobe_ = 0; mid_ = 0; sps_ = spsNom_;
        agc = 0.1f; carrierLock = 0; snrDb = 0; meanAbs_ = 0; varAbs_ = 0;
        cons.clear(); consPos = 0;
    }
    void reacquire() { acquired = (mod == Fsk); acq_.clear(); }
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
    double spsNom_ = 2, sps_ = 2, alpha_ = 0.5, cA_ = 0.01, cB_ = 1e-4, tBl_ = 0.005;
    double phase_ = 0, freq_ = 0, t_ = 1;
    std::complex<double> rotD_{1, 0};
    cfp rot_{1, 0}, step_{1, 0};
    uint32_t renorm_ = 0;
    cfp h_[4];
    int strobePh_ = 0;
    cfp lastStrobe_{0, 0}, mid_{0, 0};
    float meanAbs_ = 0, varAbs_ = 0;
    cfp prevFsk_{1, 0};
    float fskDc_ = 0, fskAvg_ = 0, prevI_ = 0;
    bool oqHave_ = false;
    void updateStep() {
        const double w = -2 * kPi * (offsetHz + cfoCoarse) / inRate;
        step_ = cfp((float)std::cos(w), (float)std::sin(w));
    }

    void acquire() {
        const int log2n = 15, N = 1 << log2n;
        std::vector<float> re((size_t)N), im((size_t)N), pow((size_t)N);
        const int mult = mod == Pm ? 1 : mod == Bpsk ? 2 : 4;   // OQPSK: the fourth power still shows the line at 4 fc (weaker)
        for (int i = 0; i < N; i++) {
            cfp v = acq_[(size_t)i];
            cfp w = mod == Pm ? v : mod == Bpsk ? v * v : (v * v) * (v * v);
            w /= std::max(1e-7f, std::abs(w));   // limits strong interference
            const float win = 0.5f - 0.5f * (float)std::cos(2 * kPi * i / N);
            re[(size_t)i] = w.real() * win; im[(size_t)i] = w.imag() * win;
        }
        fftSplit(re.data(), im.data(), log2n, false);
        for (int i = 0; i < N; i++) pow[(size_t)i] = re[(size_t)i] * re[(size_t)i] + im[(size_t)i] * im[(size_t)i];
        const double maxHz = std::min(0.45 * decRate / mult * 1.0, mod == Pm ? 0.45 * sym : 0.45 * decRate / mult);
        const int maxBin = (int)(maxHz * mult / decRate * N);
        int best = 0; float bv = 0;
        double total = 0;
        for (int k = -maxBin; k <= maxBin; k++) { const float v = pow[(size_t)((k + N) % N)]; total += v; if (v > bv) { bv = v; best = k; } }
        acqPeak = bv / std::max(1e-9, total / (2 * maxBin + 1));
        double delta = 0;
        if (acqPeak > 6) {
            const float l = pow[(size_t)((best - 1 + N) % N)], c = bv, r = pow[(size_t)((best + 1 + N) % N)];
            const double d = 2 * c - l - r;
            delta = d > 0 ? 0.5 * (r - l) / d : 0;
            cfoCoarse += (best + delta) * decRate / N / mult;
        }
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
        cfp v;
        if (mod == Fsk) {                                        // discriminator, a slow mean for the Doppler, the matched filter
            const cfp d = y * std::conj(prevFsk_);
            prevFsk_ = y;
            float f = std::atan2(d.imag(), d.real()) * (float)(decRate / (kPi * sym));
            const float dcA = (float)std::min(0.5, 1.0 / (60.0 * spsNom_));
            fskDc_ += dcA * (f - fskDc_);
            f -= fskDc_;
            fskAvg_ += 0.002f * (std::fabs(f) - fskAvg_);
            carrierLock += 0.0005f * (std::min(1.f, fskAvg_ * 2.f) - carrierLock);
            h_[0] = h_[1]; h_[1] = h_[2]; h_[2] = h_[3]; h_[3] = rrc_.push(cfp(f, 0), false);
            t_ -= 1.0;
            while (t_ <= 0) {
                const float dt = (float)-t_;
                const float mu = 1.f - dt;
                const cfp a = interp(mu);
                event(a, sink);
            }
            return;
        }
        // carrier
        const cfp nco((float)std::cos(phase_), (float)-std::sin(phase_));
        v = y * nco;
        if (mod == Pm) {
            const float e = std::atan2(v.imag(), v.real());
            freq_ += cB_ * e * 0.5f;
            phase_ += freq_ + cA_ * e * 0.5f;
            carrierLock += 0.0005f * (std::cos(e) * std::cos(e) - carrierLock);
            v = rrc_.push(cfp(v.imag(), 0), false);
        } else {
            phase_ += freq_;
            v = rrc_.push(v, true);
        }
        if (phase_ > 2 * kPi) phase_ -= 2 * kPi; else if (phase_ < -2 * kPi) phase_ += 2 * kPi;
        h_[0] = h_[1]; h_[1] = h_[2]; h_[2] = h_[3]; h_[3] = v;
        t_ -= 1.0;
        while (t_ <= 0) {
            const float d = (float)-t_;
            const float mu = 1.f - d;
            const cfp a = interp(mu);
            event(a, sink);
        }
    }

    cfp interp(float mu) const {
        const float c0 = -mu * (mu - 1) * (mu - 2) / 6, c1 = (mu + 1) * (mu - 1) * (mu - 2) / 2, c2 = -(mu + 1) * mu * (mu - 2) / 2, c3 = (mu + 1) * mu * (mu - 1) / 6;
        return h_[0] * c0 + h_[1] * c1 + h_[2] * c2 + h_[3] * c3;
    }

    template<class S> void event(cfp a, S&& sink) {
        if (strobePh_ == 0) { mid_ = a; strobePh_ = 1; t_ += sps_ * 0.5; return; }
        strobePh_ = 0;
        const float mag2 = std::max(1e-9f, std::abs(a) * std::abs(a));
        agc += 0.002f * (std::sqrt(mag2 / (mod == Qpsk ? 2 : 1)) - agc);
        const float midI = mid_.real(), midQ = mid_.imag();
        // Gardner
        float e;
        if (mod == Qpsk) e = (lastStrobe_.real() - a.real()) * midI + (lastStrobe_.imag() - a.imag()) * midQ;
        else e = (lastStrobe_.real() - a.real()) * midI;
        e /= std::max(1e-6f, agc * agc);
        lastStrobe_ = a;
        e = std::clamp(e, -1.f, 1.f);
        double wA, wB;
        loopGains(tBl_, 0.9, wA, wB);
        sps_ = std::clamp(sps_ + wB * e * spsNom_, spsNom_ * 0.992, spsNom_ * 1.008);
        t_ += sps_ * 0.5 + wA * e * spsNom_ * 0.5;
        // carrier (BPSK, QPSK)
        const float I = a.real() / agc;
        const float Q = (mod == Oqpsk ? midQ : a.imag()) / agc;   // OQPSK: the Q rail is half a symbol away from I
        if (mod == Fsk) { const float snrAbs = std::fabs(I); meanAbs_ += 0.003f * (snrAbs - meanAbs_); varAbs_ += 0.003f * ((snrAbs - meanAbs_) * (snrAbs - meanAbs_) - varAbs_); snrDb = 10.f * (float)std::log10(std::max(1e-3f, meanAbs_ * meanAbs_ / std::max(1e-6f, 2 * varAbs_))); if (cons.size() < 256) cons.emplace_back(I, 0.f); else { cons[consPos] = {I, 0.f}; consPos = (consPos + 1) % 256; } sink(I, 0.f); return; }
        if (mod != Pm) {
            const float ec = mod == Bpsk ? std::clamp((I > 0 ? 1.f : -1.f) * Q, -1.f, 1.f)
                                         : std::clamp((I > 0 ? 1.f : -1.f) * Q - (Q > 0 ? 1.f : -1.f) * I, -1.f, 1.f);
            freq_ += cB_ * ec / spsNom_;
            phase_ += cA_ * ec;
            carrierLock += 0.002f * ((1.f - std::fabs(ec)) - carrierLock);
        }
        const float ab = (mod == Qpsk || mod == Oqpsk) ? 0.5f * (std::fabs(I) + std::fabs(Q)) : std::fabs(I);
        meanAbs_ += 0.003f * (ab - meanAbs_);
        varAbs_ += 0.003f * ((ab - meanAbs_) * (ab - meanAbs_) - varAbs_);
        snrDb = 10.f * (float)std::log10(std::max(1e-3f, meanAbs_ * meanAbs_ / std::max(1e-6f, 2 * varAbs_)));
        const bool two = mod == Qpsk || mod == Oqpsk;
        if (cons.size() < 256) cons.emplace_back(I, two ? Q : 0.f);
        else { cons[consPos] = {I, two ? Q : 0.f}; consPos = (consPos + 1) % 256; }
        if (mod == Oqpsk) { if (oqHave_) sink(prevI_, Q); prevI_ = I; oqHave_ = true; }   // I of the last strobe with the Q between the strobes
        else sink(I, Q);
    }
};


} // namespace dect2::sd3
