// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FM multiplex power per ITU-R BS.412 (see mpx412.h).
#include "dect2/smart5/mpx412.h"
#include <algorithm>

namespace dect2 {
namespace smart5 {

void Mpx412Meter::configure(double rateHz, double windowSec) {
    rate_ = rateHz; win_ = windowSec;
    perWin_ = rateHz > 0 ? (uint64_t)std::llround(rateHz * windowSec) : 0;
    n_ = 0; sum2_ = 0; hist_.fill(0); over_ = 0; peak_ = 0; r_ = {};
}

void Mpx412Meter::push(const float* d, size_t n) {
    if (!perWin_) return;
    for (size_t i = 0; i < n; i++) {
        const double v = d[i], a = std::fabs(v);
        sum2_ += v * v;
        hist_[std::min(kBins - 1, (int)(a / 500.0))]++;
        if (a > 75000.0) over_++;
        if (a > peak_) peak_ = a;
        if (++n_ >= perWin_) finishWindow();
    }
    updateRunning();
}

void Mpx412Meter::updateRunning() {
    if (!n_) return;
    r_.windowFill = (double)n_ / perWin_;
    if ((double)n_ >= rate_) r_.runningDbr = dbr(sum2_ / n_);
}

void Mpx412Meter::finishWindow() {
    r_.valid = true;
    r_.mpxPowerDbr = dbr(sum2_ / n_);
    r_.runningDbr = r_.mpxPowerDbr;
    auto pct = [&](double q) {
        const uint64_t want = (uint64_t)std::ceil(q * n_);
        uint64_t c = 0;
        for (int b = 0; b < kBins; b++) { c += hist_[b]; if (c >= want) return (b + 1) * 0.5; }
        return kBins * 0.5;
    };
    r_.p50Khz = pct(0.5); r_.p99Khz = pct(0.99); r_.p999Khz = pct(0.999);
    r_.maxKhz = peak_ / 1000.0;
    r_.over75Pct = 100.0 * over_ / n_;
    n_ = 0; sum2_ = 0; hist_.fill(0); over_ = 0; peak_ = 0;
    r_.windowFill = 0;
}

} // namespace smart5
} // namespace dect2
