// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/engine/widespan.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace { std::atomic<double> gWideCap{0}; }
double wideSpanCapHz() { const double c = gWideCap.load(); return c > 0 ? c : kWideMaxSpanHz; }
void setWideSpanCapHz(double hz) { gWideCap = hz > 0 ? hz : 0; }

void WideDecimator::configure(double inRate, double spanHz) {
    fs_ = inRate;
    span_ = std::max(1000.0, std::min(spanHz, wideSpanCapHz()));
    const double want = std::max(1.5 * span_, 260000.0);
    dec_ = std::max(1, (int)std::floor(fs_ / want));
    outRate_ = fs_ / dec_;
    span_ = std::min(span_, 0.8 * outRate_);
    h_.clear();
    if (dec_ > 1) {
        const double edgeLo = span_ / 2, edgeHi = outRate_ - span_ / 2;   // pass to the span edge, stop where it would fold back onto it
        const double fc = 0.5 * (edgeLo + edgeHi) / fs_, tr = (edgeHi - edgeLo) / fs_;
        int n = (int)std::ceil(4.0 / tr) | 1;
        n = std::max(n, 2 * dec_ + 1);
        using dect2::math::kPi;
        double s = 0;
        h_.resize((size_t)n);
        for (int i = 0; i < n; i++) {
            const double m = i - (n - 1) / 2.0;
            const double sinc = m == 0 ? 2 * fc : std::sin(2 * kPi * fc * m) / (kPi * m);
            const double w = 0.42 - 0.5 * std::cos(2 * kPi * i / (n - 1)) + 0.08 * std::cos(4 * kPi * i / (n - 1));
            h_[(size_t)i] = (float)(sinc * w); s += sinc * w;
        }
        for (float& v : h_) v = (float)(v / s);
    }
    setOffsetHz(off_);
    reset();
}

void WideDecimator::setOffsetHz(double hz) {
    off_ = hz;
    if (fs_ > 0) { const double w = -2.0 * dect2::math::kPi * hz / fs_; step_ = cf32((float)std::cos(w), (float)std::sin(w)); }
}

void WideDecimator::reset() { rot_ = cf32(1.f, 0.f); rotCount_ = 0; buf_.clear(); buf_.resize(h_.size()); pos_ = 0; }

void WideDecimator::process(const cf32* x, size_t n, std::vector<cf32>& out) {
    if (fs_ <= 0) return;
    float rr = rot_.real(), ri = rot_.imag();
    const float sr = step_.real(), si = step_.imag();
    if (dec_ == 1) {
        for (size_t i = 0; i < n; i++) {
            const float xr = x[i].real(), xi = x[i].imag();
            out.push_back(cf32(xr * rr - xi * ri, xr * ri + xi * rr));
            const float nr = rr * sr - ri * si; ri = rr * si + ri * sr; rr = nr;
            if (++rotCount_ >= 1024) { rotCount_ = 0; const float g = 1.f / std::sqrt(rr * rr + ri * ri); rr *= g; ri *= g; }
        }
        rot_ = cf32(rr, ri);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        const float xr = x[i].real(), xi = x[i].imag();
        buf_.push_back(cf32(xr * rr - xi * ri, xr * ri + xi * rr));
        const float nr = rr * sr - ri * si; ri = rr * si + ri * sr; rr = nr;
        if (++rotCount_ >= 1024) { rotCount_ = 0; const float g = 1.f / std::sqrt(rr * rr + ri * ri); rr *= g; ri *= g; }
    }
    rot_ = cf32(rr, ri);
    const size_t N = h_.size();
    while (pos_ + N <= buf_.size()) {
        const cf32* b = &buf_[pos_];
        float ar = 0, ai = 0;
        for (size_t k = 0; k < N; k++) { ar += h_[k] * b[k].real(); ai += h_[k] * b[k].imag(); }
        out.push_back(cf32(ar, ai));
        pos_ += (size_t)dec_;
    }
    if (pos_ > 65536) { buf_.erase(buf_.begin(), buf_.begin() + (long)pos_); pos_ = 0; }
}

} // namespace dect2
