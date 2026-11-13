// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A baseband impulse noise blanker (#264), the usual kind: the mean magnitude of the samples is followed with a slow average, and a sample
// more than threshold times above it starts a blank of width seconds (the samples are set to zero; the average does not learn from them).
// Ignition, power-line and switching clicks are much shorter and much stronger than any wanted signal, so a blank of some tens of
// microseconds removes them and costs the signal little. Off by default; header-only, tested in tests/dsp/test_noise_blanker.cpp.
#pragma once
#include "dect2/common/ring.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace dect2 {

class NoiseBlanker {
public:
    static constexpr float kMinThreshold = 2.f, kMaxThreshold = 40.f;     // times the mean magnitude
    static constexpr double kMinWidthSec = 2e-6, kMaxWidthSec = 2e-3;     // 2 us .. 2 ms
    // threshold: times the mean magnitude; widthSec: the blank after a click; rate: samples a second
    void set(float threshold, double widthSec, double rate) {
        thr_ = std::min(kMaxThreshold, std::max(kMinThreshold, threshold));
        const double w = std::min(kMaxWidthSec, std::max(kMinWidthSec, widthSec));
        width_ = rate > 0 ? std::max<int64_t>(1, (int64_t)std::llround(w * rate)) : 1;
        const double tau = rate > 0 ? std::max(64.0, rate * 0.01) : 1024.0;   // the average: about 10 ms
        alpha_ = 1.0 / tau;
    }
    void reset() { avg_ = 0; left_ = 0; }
    void process(cf32* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            const double m = std::abs(x[i]);
            if (avg_ <= 0) { avg_ = m; seen_++; continue; }   // the first sample starts the average
            if (m > thr_ * avg_) { if (!inBlank_) events_++; left_ = width_; }
            if (left_ > 0) {
                x[i] = cf32(0, 0); left_--; blanked_++; inBlank_ = left_ > 0;
            } else {
                inBlank_ = false;
                avg_ += (m - avg_) * alpha_;
            }
            seen_++;
        }
    }
    uint64_t blanked() const { return blanked_; }   // samples set to zero so far
    uint64_t seen() const { return seen_; }
    uint64_t events() const { return events_; }      // blanks started (a click inside a blank stretches it, not counted again)
    int64_t widthSamples() const { return width_; }
    double mean() const { return avg_; }

private:
    float thr_ = 8.f;
    int64_t width_ = 1, left_ = 0;
    double alpha_ = 1.0 / 1024, avg_ = 0;
    bool inBlank_ = false;
    uint64_t blanked_ = 0, seen_ = 0, events_ = 0;
};

} // namespace dect2
