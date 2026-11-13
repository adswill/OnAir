// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// See channel_find.h.
#include "dect2/channels/channel_find.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

void ChannelCentre::configure(double rate, double bwHz, double keepWithinHz, double waitSec, double measureSec) {
    rate_ = rate; bw_ = bwHz; keep_ = keepWithinHz; wait_ = waitSec; measure_ = measureSec;
    n_ = 256;
    while (n_ < 65536 && rate / n_ > bwHz / 40) n_ *= 2;   // at least 40 bins across the channel
    reset();
}

void ChannelCentre::reset() {
    blk_.assign((size_t)n_, cf32(0, 0));
    acc_.assign((size_t)n_, 0.0);
    fill_ = 0; seen_ = 0;
    offset_ = 0; ph_ = 0; dph_ = 0;
    changed_ = false; haveCand_ = false; measured_ = false; cand_ = 0;
    cands_.clear(); tryIdx_ = 0; trying_ = false; phaseSeen_ = 0; homeLen_ = (size_t)std::max(1.0, wait_ * rate_);
    decided_ = rate_ <= 0 || bw_ <= 0 || bw_ >= rate_;
}

void ChannelCentre::process(cf32* x, size_t n, bool locked) {
    if (!decided_) {
        if (locked) decided_ = true;   // the receiver found it where it is
        for (size_t i = 0; i < n && !decided_; i++) {
            seen_++;
            if (!measured_) {
                const cf32 v = x[i];
                blk_[fill_++] = std::isfinite(v.real()) && std::isfinite(v.imag()) ? v : cf32(0, 0);
                if (fill_ == (size_t)n_) {
                    Fft f(n_);
                    for (int k = 0; k < n_; k++) blk_[(size_t)k] *= (float)(0.5 - 0.5 * std::cos(2 * M_PI * k / n_));
                    f.forward(blk_.data());
                    for (int k = 0; k < n_; k++) acc_[(size_t)k] += std::norm(blk_[(size_t)k]);
                    fill_ = 0;
                    if ((double)seen_ >= measure_ * rate_) { measure(); measured_ = true; if (!haveCand_) decided_ = true; }
                }
            }
            if (retry_ && measured_ && !decided_ && !cands_.empty()) {
                // centre -> candidate (dwell) -> centre (twice as long) -> next candidate ...
                phaseSeen_++;
                if (!trying_ && phaseSeen_ >= homeLen_) {
                    trying_ = true; phaseSeen_ = 0;
                    offset_ = cands_[tryIdx_ % cands_.size()];
                    dph_ = -2 * M_PI * offset_ / rate_; ph_ = 0; changed_ = true;
                } else if (trying_ && (double)phaseSeen_ >= dwell_ * rate_) {
                    trying_ = false; phaseSeen_ = 0; tryIdx_++;
                    homeLen_ = std::min(homeLen_ * 2, (size_t)(16.0 * rate_));
                    offset_ = 0; dph_ = 0; ph_ = 0; changed_ = true;
                }
                continue;
            }
            if (!retry_ && measured_ && !decided_ && (double)seen_ >= wait_ * rate_) {
                decided_ = true;
                offset_ = cand_;
                dph_ = -2 * M_PI * offset_ / rate_;
                ph_ = 0;
                changed_ = true;
            }
        }
    }
    if (dph_ != 0) {
        for (size_t i = 0; i < n; i++) {
            x[i] *= cf32((float)std::cos(ph_), (float)std::sin(ph_));
            ph_ += dph_;
            if (ph_ > M_PI) ph_ -= 2 * M_PI; else if (ph_ < -M_PI) ph_ += 2 * M_PI;
        }
    }
}

void ChannelCentre::measure() {
    const int N = n_;
    const double hz = rate_ / N;
    // spectrum with the centre in the middle; the DC spike of the radio (three bins) is replaced by its neighbours
    std::vector<double> s((size_t)N);
    for (int k = 0; k < N; k++) s[(size_t)k] = acc_[(size_t)((k + N / 2) % N)];
    const int c = N / 2;
    for (int d = -1; d <= 1; d++) s[(size_t)(c + d)] = 0.5 * (s[(size_t)(c - 3)] + s[(size_t)(c + 3)]);
    // the power in a window of the channel's width, at every position where it fits in 96 % of the band (the radio's filter rolls off at the edges)
    const int w = std::max(1, (int)std::lround(bw_ / hz));
    const int lo = (int)(0.02 * N), hi = (int)(0.98 * N) - w;
    if (hi <= lo) return;
    std::vector<double> pre((size_t)N + 1, 0.0);
    for (int k = 0; k < N; k++) pre[(size_t)k + 1] = pre[(size_t)k] + s[(size_t)k];
    auto box = [&](int a) { return pre[(size_t)(a + w)] - pre[(size_t)a]; };
    int best = lo; double bestP = -1;
    for (int a = lo; a <= hi; a++) {
        const double p = box(a);
        if (p > bestP) { bestP = p; best = a; }
    }
    // the floor: a low percentile of the bins (the channel may fill most of the band)
    std::vector<double> bins(s.begin() + lo, s.begin() + hi + w);
    std::nth_element(bins.begin(), bins.begin() + (long)bins.size() / 10, bins.end());
    const double floorP = bins[bins.size() / 10] * w;
    if (bestP < 4.0 * floorP) return;               // nothing stands out: leave it to the receiver
    // the middle of the channel: its half-power edges in a lightly smoothed spectrum, searched outwards from the strongest window
    auto sm = [&](int k) { k = std::max(2, std::min(N - 3, k)); return (s[(size_t)k - 2] + s[(size_t)k - 1] + s[(size_t)k] + s[(size_t)k + 1] + s[(size_t)k + 2]) / 5; };
    auto refine = [&](int at, double power, double& centreHz) {
        const double level = power / w, half = 0.5 * level;
        int a = at + w / 2, b = at + w / 2;
        while (a > 2 && a > at - w && sm(a - 1) > half) a--;
        while (b < N - 3 && b < at + 2 * w && sm(b + 1) > half) b++;
        centreHz = ((a + b) / 2.0 - c) * hz;
        return b - a >= w / 3;   // a carrier or a narrow interferer is not a channel
    };
    double cand = 0;
    const bool wide = refine(best, bestP, cand);
    cand_ = cand;
    haveCand_ = std::fabs(cand_) > keep_ && wide;
    if (!retry_ || !haveCand_) return;
    // the next strongest blocks that stand out and do not overlap the ones taken (the first is cand_)
    cands_.assign(1, cand_);
    std::vector<int> taken(1, best);
    for (int pick = 0; pick < 2; pick++) {
        int b2 = -1; double p2 = -1;
        for (int a2 = lo; a2 <= hi; a2++) {
            bool clash = false;
            for (int t : taken) if (std::abs(a2 - t) < w) { clash = true; break; }
            if (clash) continue;
            const double p = box(a2);
            if (p > p2) { p2 = p; b2 = a2; }
        }
        if (b2 < 0 || p2 < 4.0 * floorP) break;
        double c2 = 0;
        if (!refine(b2, p2, c2) || std::fabs(c2) <= keep_) break;
        taken.push_back(b2);
        cands_.push_back(c2);
    }
}

} // namespace dect2
