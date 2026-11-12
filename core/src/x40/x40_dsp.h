// A few DSP helpers shared by the second block of 4.0 signal decoders.
#pragma once
#include "x40_int.h"

namespace dect2::x40 {

// In-place radix-2 FFT of a power-of-two length
inline void fftx(std::vector<cf32>& a) {
    const int n = (int)a.size();
    for (int i = 1, j = 0; i < n; i++) { int bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(a[i], a[j]); }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2 * kPi / len;
        const cf32 wl((float)std::cos(ang), (float)std::sin(ang));
        for (int i = 0; i < n; i += len) {
            cf32 w(1, 0);
            for (int k = 0; k < len / 2; k++) { const cf32 u = a[i + k], v = a[i + k + len / 2] * w; a[i + k] = u + v; a[i + k + len / 2] = u - v; w *= wl; }
        }
    }
}

// Is there a carrier? One narrow line of the complex baseband that holds a share of the channel's power which noise alone cannot give. The look is
// one windowed FFT of about 0.2 - 0.4 s; the line is the strongest 5 bins (a Hann main lobe). Noise alone puts about 17 / n of the power in its
// best 5 bins (2e-3 at n = 8192), a carrier 15 dB under the rest of the channel 3e-2: the threshold 0.012 sits between. present() is the verdict of the last look
// (false until the first one). Without this an AM monitor "found a carrier" in white noise (QA sweep 4.0).
class CarrierGate {
public:
    void configure(double rate, double minFraction = 0.012) {
        n_ = 256; while (n_ * 2 <= rate * 0.4 && n_ < 32768) n_ *= 2;
        thr_ = minFraction; w_.resize((size_t)n_);
        for (int i = 0; i < n_; i++) w_[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / n_));
        reset();
    }
    void reset() { buf_.clear(); present_ = false; fraction_ = 0; }
    void push(cf32 a) { buf_.push_back(a); if ((int)buf_.size() >= n_) look(); }
    bool present() const { return present_; }
    float fraction() const { return fraction_; }
private:
    void look() {
        std::vector<cf32> f((size_t)n_);
        for (int i = 0; i < n_; i++) f[(size_t)i] = buf_[(size_t)i] * w_[(size_t)i];
        buf_.clear();
        fftx(f);
        std::vector<double> p((size_t)n_);
        double tot = 0;
        for (int k = 0; k < n_; k++) { p[(size_t)k] = std::norm(f[(size_t)k]); tot += p[(size_t)k]; }
        double best = 0;
        for (int k = 0; k < n_; k++) { double q = 0; for (int d = -2; d <= 2; d++) q += p[(size_t)((k + d + n_) % n_)]; best = std::max(best, q); }
        fraction_ = tot > 0 ? (float)(best / tot) : 0.f;
        present_ = fraction_ > thr_;
    }
    int n_ = 256; double thr_ = 0.012; bool present_ = false; float fraction_ = 0;
    std::vector<float> w_; std::vector<cf32> buf_;
};

// Welch power spectrum of a real signal: one-sided band powers by frequency. psd(k) = mean squared amplitude in bin k (so a sine of amplitude A has A^2 / 2 summed over its bins).
class RealWelch {
public:
    void configure(double rate, int n) {
        rate_ = rate; n_ = n; w_.resize(n); double s2 = 0;
        for (int i = 0; i < n; i++) { w_[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / n)); s2 += (double)w_[i] * w_[i]; }
        norm_ = 1.0 / ((double)n * s2); acc_.assign(n / 2 + 1, 0.0); cnt_ = 0; buf_.assign(n, cf32());
    }
    int bins() const { return n_ / 2 + 1; }
    double binHz() const { return rate_ / n_; }
    int segments() const { return cnt_; }
    void clear() { std::fill(acc_.begin(), acc_.end(), 0.0); cnt_ = 0; }
    // x: n real samples
    void add(const float* x) {
        for (int i = 0; i < n_; i++) buf_[i] = cf32(x[i] * w_[i], 0);
        fftx(buf_);
        for (int k = 0; k <= n_ / 2; k++) acc_[k] += (double)std::norm(buf_[k]) * norm_ * (k == 0 || k == n_ / 2 ? 1.0 : 2.0);
        cnt_++;
    }
    double band(double f0, double f1) const {     // mean squared amplitude between f0 and f1 (Hz)
        if (!cnt_) return 0;
        const int a = std::max(0, (int)std::floor(f0 / binHz())), b = std::min(n_ / 2, (int)std::ceil(f1 / binHz()));
        double s = 0; for (int k = a; k <= b; k++) s += acc_[k];
        return s / cnt_;
    }
private:
    double rate_ = 1, norm_ = 1; int n_ = 0, cnt_ = 0; std::vector<float> w_; std::vector<double> acc_; std::vector<cf32> buf_;
};

} // namespace dect2::x40
