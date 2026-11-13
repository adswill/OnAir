// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Real samples at fsHz -> complex baseband: a decimating FIR with complex taps. Shared by the native radios that sample a real signal
// directly (the Fobos SDR's HF inputs, the RX888's ADC): source_fobos.cpp, source_rx888.cpp.
#pragma once
#include "native_common.h"
#include "dect2/dsp/dsp_compat.h"
#include "dect2/common/window.h"

namespace dect2 {
namespace native {

// Zeroth-order modified Bessel function (Kaiser window)
inline double besselI0(double x) { return dect2::window::besselI0(x, 64, 1e-17); }

// A real input at fsHz -> complex baseband around fcHz at fsHz / decim.
//   y[m] = sum_k h[k] x[mD + k] exp(-jw(mD + k)) = exp(-jwmD) * sum_k (h[k] exp(-jwk)) x[mD + k],   w = 2 pi fc / fs
// so one decimating FIR with complex taps (two real ones, dsp_compat's desamp) and one rotation per output sample. h is a Kaiser low-pass,
// 16 taps per output sample: flat to +-0.35 of the output rate, 75 dB down from 0.65 on, so what folds back lands outside the middle 70 %.
// A sine at the input's full scale comes out as a complex tone of the same amplitude.
class RealDdc {
public:
    int decim() const { return D_; }
    void design(int decim) {
        D_ = std::max(1, decim);
        if (D_ == 1) {   // no decimation (RX888 above half its clock): a plain mixer, one tap, the whole real band with its mirror image
            N_ = 1;
            h_.assign(1, 1.0);
            in_.clear();
            ph_ = {1, 0};
            return;
        }
        N_ = 16 * D_;
        h_.assign((size_t)N_, 0.0);
        const double beta = 7.5, cut = 0.5 / D_, mid = (N_ - 1) / 2.0, i0b = besselI0(beta);
        double sum = 0;
        for (int k = 0; k < N_; k++) {
            const double t = k - mid, r = t / (mid + 0.5);
            const double sinc = std::fabs(t) < 1e-9 ? 2 * cut : std::sin(2 * M_PI * cut * t) / (M_PI * t);
            h_[(size_t)k] = sinc * besselI0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0b;
            sum += h_[(size_t)k];
        }
        for (auto& v : h_) v /= sum;
        in_.clear();
        ph_ = {1, 0};
    }
    void tune(double fsHz, double fcHz, double gain) {
        const double w = 2 * M_PI * fcHz / fsHz;
        gr_.resize((size_t)N_); gi_.resize((size_t)N_);
        for (int k = 0; k < N_; k++) {
            const double a = std::fmod(w * k, 2 * M_PI);
            gr_[(size_t)k] = (float)(2 * gain * h_[(size_t)k] * std::cos(a));
            gi_[(size_t)k] = (float)(-2 * gain * h_[(size_t)k] * std::sin(a));
        }
        step_ = std::polar(1.0, -std::fmod(w * D_, 2 * M_PI));
        mixGain_ = (float)(2 * gain);
        w_ = w;
        stepBlock_ = std::polar(1.0, -std::fmod(w * (double)kMixBlock, 2 * M_PI));
    }
    // x: n samples, one every `stride` floats; the output is appended to out
    void process(const float* x, size_t n, size_t stride, std::vector<cf32>& out) {
        const size_t have = in_.size();
        in_.resize(have + n);
        for (size_t i = 0; i < n; i++) in_[have + i] = x[i * stride];
        filter(out);
    }
    // 16-bit samples as they come from a converter (RX888): the float values are the integers, so tune() gets gain / 32768 for a full scale of 1.
    // derand: the converter's output randomiser is on (LSB set: every other bit is inverted), undone here.
    void processS16(const int16_t* x, size_t n, bool derand, std::vector<cf32>& out) {
        if (D_ == 1) { mixS16(x, n, derand, out); return; }
        const size_t have = in_.size();
        in_.resize(have + n);
        float* d = in_.data() + have;
        if (derand) for (size_t i = 0; i < n; i++) { const int16_t v = x[i]; d[i] = (float)(int16_t)((v & 1) ? (v ^ -2) : v); }
        else for (size_t i = 0; i < n; i++) d[i] = (float)x[i];
        filter(out);
    }

private:
    // D = 1: no filter, y[n] = 2 gain x[n] exp(-jwn). The phasor runs in float within blocks of kMixBlock samples and is re-anchored from a
    // double one at each block start, so its error does not grow (the RX888 at 128 Msps spends most of its time here).
    static constexpr size_t kMixBlock = 1024;
    void mixS16(const int16_t* x, size_t n, bool derand, std::vector<cf32>& out) {
        const size_t at = out.size();
        out.resize(at + n);
        cf32* o = out.data() + at;
        const float sr = (float)step_.real(), si = (float)step_.imag(), g = mixGain_;
        for (size_t i = 0; i < n;) {
            const size_t len = std::min(kMixBlock, n - i);
            float pr = (float)ph_.real(), pi = (float)ph_.imag();
            for (size_t k = 0; k < len; k++) {
                const int16_t v = x[i + k];
                const float s = g * (float)(derand && (v & 1) ? (int16_t)(v ^ -2) : v);
                o[i + k] = cf32(s * pr, s * pi);
                const float nr = pr * sr - pi * si;
                pi = pr * si + pi * sr;
                pr = nr;
            }
            ph_ *= len == kMixBlock ? stepBlock_ : std::polar(1.0, -std::fmod(w_ * (double)len, 2 * M_PI));
            ph_ /= std::abs(ph_);
            i += len;
        }
    }
    void filter(std::vector<cf32>& out) {
        if (in_.size() < (size_t)N_) return;
        const size_t m = (in_.size() - (size_t)N_) / (size_t)D_ + 1;
        yr_.resize(m); yi_.resize(m);
        desamp(in_.data(), D_, gr_.data(), yr_.data(), (int)m, N_);
        desamp(in_.data(), D_, gi_.data(), yi_.data(), (int)m, N_);
        const size_t at = out.size();
        out.resize(at + m);
        std::complex<double> ph = ph_;
        for (size_t i = 0; i < m; i++) {
            out[at + i] = cf32((float)(yr_[i] * ph.real() - yi_[i] * ph.imag()), (float)(yr_[i] * ph.imag() + yi_[i] * ph.real()));
            ph *= step_;
        }
        ph_ = ph / std::abs(ph);
        in_.erase(in_.begin(), in_.begin() + (std::ptrdiff_t)(m * (size_t)D_));
    }

    int D_ = 1, N_ = 16;
    std::vector<double> h_;
    std::vector<float> gr_, gi_, in_, yr_, yi_;
    std::complex<double> ph_{1, 0}, step_{1, 0}, stepBlock_{1, 0};
    double w_ = 0;
    float mixGain_ = 2;
};

} // namespace native
} // namespace dect2
