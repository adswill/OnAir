// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared first stage for receivers that watch several channels inside one capture: the capture is mixed to the middle of the channel group and
// low-passed / decimated ONCE (a polyphase FIR, the SIMD desamp kernel), and every channel then runs at the low rate. Before this each channel
// mixed and filtered every input sample on its own, which cost (channels x input rate); now it costs (input rate) plus (channels x output rate).
#pragma once
#include "dect2/common/ring.h"
#include "dect2/dsp/dsp_compat.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace dect2 {

class GroupDecimator {
public:
    // rate: input rate; midHz: the middle of the group (Hz from the capture's centre, where the channels' offsets are measured); halfSpanHz: half the
    // width of the group including every channel's own bandwidth; D: the decimation. Returns false (and stays off) when the transition band left
    // between halfSpanHz and rate / D - halfSpanHz is narrower than minTransHz.
    bool configure(double rate, double midHz, double halfSpanHz, int D, double minTransHz = 20e3, int maxTaps = 1201) {
        on_ = false;
        const double fo = rate / D, stop = fo - halfSpanHz;
        if (D < 2 || stop - halfSpanHz < minTransHz) return false;
        int nt = (int)std::ceil(5.5 * rate / (stop - halfSpanHz));
        nt = std::min(nt, maxTaps) | 1;
        const double cutoff = 0.5 * (halfSpanHz + stop) / rate;       // Blackman windowed sinc, unity gain at DC
        h_.resize((size_t)nt);
        double sum = 0;
        const int m = nt / 2;
        for (int i = 0; i < nt; i++) {
            const double k = i - m;
            const double s = k == 0 ? 2 * cutoff : std::sin(2 * math::kPi * cutoff * k) / (math::kPi * k);
            const double w = 0.42 - 0.5 * std::cos(2 * math::kPi * i / (nt - 1)) + 0.08 * std::cos(4 * math::kPi * i / (nt - 1));
            h_[(size_t)i] = (float)(s * w); sum += s * w;
        }
        for (auto& v : h_) v = (float)(v / sum);
        D_ = D; outRate_ = fo;
        re_.assign(h_.size() - 1, 0.f); im_.assign(h_.size() - 1, 0.f);
        st_ = 0; ph_ = std::complex<double>(1, 0);
        step_ = std::polar(1.0, -2 * math::kPi * midHz / rate);
        on_ = true;
        return true;
    }
    bool on() const { return on_; }
    double outRate() const { return outRate_; }
    // n input samples in; the decimated samples are returned (valid until the next call)
    const std::vector<cf32>& process(const cf32* x, size_t n) {
        const size_t L = h_.size(), H = L - 1;
        re_.resize(H + n); im_.resize(H + n);
        float* re = re_.data() + H;
        float* im = im_.data() + H;
        for (size_t i = 0; i < n; i++) {
            cf32 v = x[i];
            if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
            const double a = ph_.real(), b = ph_.imag();
            re[i] = (float)(v.real() * a - v.imag() * b); im[i] = (float)(v.real() * b + v.imag() * a);
            ph_ = std::complex<double>(a * step_.real() - b * step_.imag(), a * step_.imag() + b * step_.real());
            if ((i & 1023) == 1023) ph_ /= std::abs(ph_);
        }
        const size_t T = H + n;
        out_.clear();
        if (T >= L + st_) {
            const size_t cnt = (T - L - st_) / (size_t)D_ + 1;
            ore_.resize(cnt); oim_.resize(cnt);
            desamp(re_.data() + st_, D_, h_.data(), ore_.data(), (int)cnt, (int)L);
            desamp(im_.data() + st_, D_, h_.data(), oim_.data(), (int)cnt, (int)L);
            out_.resize(cnt);
            for (size_t i = 0; i < cnt; i++) out_[i] = cf32(ore_[i], oim_[i]);
            st_ = st_ + cnt * (size_t)D_ - n;
        } else {
            st_ -= n;
        }
        std::copy(re_.begin() + (std::ptrdiff_t)n, re_.end(), re_.begin());
        std::copy(im_.begin() + (std::ptrdiff_t)n, im_.end(), im_.begin());
        re_.resize(H); im_.resize(H);
        return out_;
    }
private:
    bool on_ = false;
    int D_ = 1;
    double outRate_ = 0;
    std::vector<float> h_, re_, im_, ore_, oim_;
    std::vector<cf32> out_;
    std::complex<double> ph_{1, 0}, step_{1, 0};
    size_t st_ = 0;
};

} // namespace dect2
