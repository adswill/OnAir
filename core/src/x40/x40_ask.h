// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A small envelope slicer for the on-off / ASK style 4.0 decoders: a hysteresis comparator between slowly decaying high and low trackers. It calls back with
// the time of every level change (interpolated to a fraction of a sample) and the level before it. A swing is only believed when it is well over the noise.
#pragma once
#include "x40_int.h"

namespace dect2::x40 {

class AskSlicer {
public:
    void configure(double rate, double trackSec = 0.06, double hystFrac = 0.22, double minSigmas = 5.0) {
        rate_ = rate; trackSec_ = trackSec; hyst_ = (float)hystFrac; sigmas_ = (float)minSigmas; reset();
    }
    void reset() { crossUp_ = crossDown_ = 0; n_ = 0; y_ = hi_ = lo_ = mean_ = swing_ = noise_ = prevE_ = 0; init_ = false; state_ = 0; }
    float carrier() const { return mean_; }
    float swing() const { return swing_; }
    bool seen() const { return init_; }
    // e: the envelope of one sample. edge(timeSec, levelBefore) is called at a change.
    template <class F> void push(float e, F&& edge) {
        n_++; const double t = n_ / rate_;
        const float yPrev = y_;
        y_ += 0.5f * (e - y_);
        mean_ += 0.0005f * (e - mean_);
        if (!init_) { init_ = true; hi_ = lo_ = y_ = yPrev_ = e; mean_ = e; prevE_ = e; return; }
        if (t < 0.02) { hi_ = lo_ = y_; mean_ = y_; prevE_ = e; return; }   // the filters before it start from zero: let the trackers begin from the settled level
        const float d = 1.f / (float)(trackSec_ * rate_);
        if (y_ > hi_) hi_ = y_; else hi_ -= d * (hi_ - y_);
        if (y_ < lo_) lo_ = y_; else lo_ += d * (y_ - lo_);
        const float span = hi_ - lo_, mid = 0.5f * (hi_ + lo_);
        const float dd = e - prevE_; prevE_ = e;
        if (std::fabs(dd) < 0.25f * span + 1e-9f) noise_ += 0.001f * (dd * dd - noise_);
        const float sigma = std::sqrt(std::max(noise_, 1e-20f) * 0.5f);
        if (span < sigmas_ * sigma + 1e-6f * std::max(mean_, 1e-6f)) { state_ = 0; return; }
        int ns = state_;
        if (y_ > mid + hyst_ * span) ns = 1; else if (y_ < mid - hyst_ * span) ns = 0;
        // the latest crossing of the middle, interpolated between two samples
        if ((yPrev >= mid) != (y_ >= mid)) { const float dy = y_ - yPrev; const double tc = std::fabs(dy) > 1e-12f ? t - (1.0 - (mid - yPrev) / dy) / rate_ : t; if (y_ >= mid) crossUp_ = tc; else crossDown_ = tc; }
        if (ns != state_) {
            double tc = ns ? crossUp_ : crossDown_; if (tc <= 0 || t - tc > 6.0 / rate_) tc = t;
            const int before = state_; state_ = ns;
            swing_ = 0.99f * swing_ + 0.01f * span;
            edge(tc, before);
        }
    }
private:
    double rate_ = 1, trackSec_ = 0.06; float hyst_ = 0.22f, sigmas_ = 5; uint64_t n_ = 0;
    double crossUp_ = 0, crossDown_ = 0; float y_ = 0, yPrev_ = 0, hi_ = 0, lo_ = 0, mean_ = 0, swing_ = 0, noise_ = 0, prevE_ = 0; bool init_ = false; int state_ = 0;
};

} // namespace dect2::x40
