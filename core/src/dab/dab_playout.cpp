// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB / DAB+ playout: concealment, splices and the rate control of the output buffer (dab_playout.h).
#include "dect2/dab/dab_playout.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

namespace {
constexpr int kHist = DabPlayout::kPeriod + 4 * DabPlayout::kXfade;   // frames of history kept
constexpr int kFadeIn = 480;                                           // 10 ms fade-in when the audio comes back after silence
constexpr double kPi = 3.14159265358979323846;
}

DabPlayout::DabPlayout() { reset(); }

void DabPlayout::reset() {
    hist_.clear();
    haveTail_ = false;
    conceal_ = false;
    ct_ = 0;
    seg_.clear(); segLen_ = 0; segPos_ = 0;
    ratio_ = 1.0; fillAvg_ = -1;
    frac_ = 0; primed_ = false;
    for (auto& f : in_) f[0] = f[1] = 0.f;
}

// the level of the concealment after ct frames of it
static float concealGain(int64_t ct) {
    if (ct < DabPlayout::kHoldFrames) return 1.f;
    const int64_t t = ct - DabPlayout::kHoldFrames;
    if (t >= DabPlayout::kFadeFrames) return 0.f;
    return (float)(0.5 * (1.0 + std::cos(kPi * (double)t / DabPlayout::kFadeFrames)));
}

bool DabPlayout::quiet() const { return conceal_ && concealGain(ct_) == 0.f; }

void DabPlayout::remember(const float* st, int n) {
    hist_.insert(hist_.end(), st, st + 2 * (size_t)n);
    if (hist_.size() > (size_t)kHist * 2) hist_.erase(hist_.begin(), hist_.end() - (long)kHist * 2);
}

// The next frame of the repeated stretch: seg_ holds kXfade frames before the stretch, then the segLen_ frames of it; the end of every pass is
// crossfaded into the frames before its start, so the next pass continues from there without a step
float DabPlayout::concealSample(int ch) {
    if (segLen_ <= 0) return 0.f;
    const int j = segPos_;
    float v = seg_[(size_t)(kXfade + j) * 2 + (size_t)ch];
    const int blend = segLen_ - kXfade;
    if (j >= blend) {
        const float a = ((float)(j - blend) + 0.5f) / (float)kXfade;
        v = v * (1.f - a) + seg_[(size_t)(j - blend) * 2 + (size_t)ch] * a;
    }
    return v;
}

void DabPlayout::lost(int n, std::vector<float>& out) {
    if (n <= 0) return;
    const int avail = (int)(hist_.size() / 2);
    if (!conceal_) {
        conceal_ = true;
        ct_ = 0;
        segLen_ = std::min(kPeriod, avail - kXfade);
        if (segLen_ < 4 * kXfade) {   // no audio to repeat yet: what is held goes out, then silence
            segLen_ = 0;
            if (haveTail_) { put(&hist_[hist_.size() - (size_t)kXfade * 2], kXfade, out); haveTail_ = false; }
        } else {
            seg_.assign(hist_.end() - (long)(segLen_ + kXfade) * 2, hist_.end());
            // the frames held back (the last kXfade of the history) are the end of the stretch: the concealment starts on them, in the crossfade
            segPos_ = haveTail_ ? segLen_ - kXfade : 0;
            haveTail_ = false;
        }
    }
    tmp_.resize((size_t)n * 2);
    for (int i = 0; i < n; i++) {
        const float g = concealGain(ct_);
        tmp_[(size_t)i * 2] = g * concealSample(0);
        tmp_[(size_t)i * 2 + 1] = g * concealSample(1);
        if (segLen_ > 0 && ++segPos_ >= segLen_) segPos_ = 0;
        if (g == 0.f) silent_++;
        ct_++;
        concealed_++;
    }
    put(tmp_.data(), n, out);
}

void DabPlayout::good(const float* st, int n, std::vector<float>& out) {
    if (n <= 0) return;
    std::vector<float> v(st, st + (size_t)n * 2);
    if (conceal_) {
        // the splice: the concealment goes on under the first frames of the new audio and is faded out against it (over 2 ms, or 10 ms when
        // the concealment had gone quiet, so the audio fades in)
        const bool quiet = concealGain(ct_) < 0.5f;
        const int m = std::min(n, quiet ? kFadeIn : kXfade);
        for (int i = 0; i < m; i++) {
            const float a = ((float)i + 0.5f) / (float)m;
            const float g = concealGain(ct_) * (1.f - a);
            v[(size_t)i * 2] = v[(size_t)i * 2] * a + g * concealSample(0);
            v[(size_t)i * 2 + 1] = v[(size_t)i * 2 + 1] * a + g * concealSample(1);
            if (segLen_ > 0 && ++segPos_ >= segLen_) segPos_ = 0;
            ct_++;
        }
        conceal_ = false;
        ct_ = 0;
        haveTail_ = false;
        hist_.clear();   // the history is the audio after the splice
    }
    // the frames held back from the last block, then this block but its last kXfade frames (held back for the next splice)
    if (haveTail_) put(&hist_[hist_.size() - (size_t)kXfade * 2], kXfade, out);
    haveTail_ = false;
    remember(v.data(), n);
    if (n > kXfade && hist_.size() >= (size_t)kXfade * 2) {
        put(v.data(), n - kXfade, out);
        haveTail_ = true;
    } else put(v.data(), n, out);
}

void DabPlayout::setFill(int fill) {
    fillAvg_ = fillAvg_ < 0 ? fill : fillAvg_ + 0.05 * ((double)fill - fillAvg_);
    const double err = ((double)kTarget - fillAvg_) / kTarget;   // > 0: too little queued, play the audio a little longer (more frames)
    ratio_ = 1.0 + std::max(-kMaxRatio, std::min(kMaxRatio, 0.01 * err));
}

// Cubic (Catmull-Rom) resampling by ratio_ output frames per input frame
void DabPlayout::put(const float* st, int n, std::vector<float>& out) {
    if (!primed_) {   // the first frames: the history starts as copies of the first one (no ramp from zero)
        if (n <= 0) return;
        for (auto& f : in_) { f[0] = st[0]; f[1] = st[1]; }
        primed_ = true;
        frac_ = 0;
    }
    const double step = 1.0 / ratio_;
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < 3; k++) { in_[k][0] = in_[k + 1][0]; in_[k][1] = in_[k + 1][1]; }
        in_[3][0] = st[(size_t)i * 2]; in_[3][1] = st[(size_t)i * 2 + 1];
        while (frac_ < 1.0) {
            const float t = (float)frac_;
            for (int c = 0; c < 2; c++) {
                const float p0 = in_[0][c], p1 = in_[1][c], p2 = in_[2][c], p3 = in_[3][c];
                const float y = p1 + 0.5f * t * (p2 - p0 + t * (2.f * p0 - 5.f * p1 + 4.f * p2 - p3 + t * (3.f * (p1 - p2) + p3 - p0)));
                out.push_back(y);
            }
            frac_ += step;
        }
        frac_ -= 1.0;
    }
}

} // namespace dect2
