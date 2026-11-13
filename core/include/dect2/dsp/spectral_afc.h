// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Carrier follow-up for narrow channel filters (4.0, docs/SENSITIVITY-4.0-A2.md). Feed it the complex baseband of one channel at a low rate
// (after the anti-alias filter, before the channel filter). The mean power spectrum is averaged over about 0.8 s, the noise floor is taken
// from its quietest quarter, and the centroid of what stands above 1.4 x the floor inside +-searchHz is how far the signal sits from the
// centre. adjustHz() is the correction to add to the mixer (never beyond +-maxHz). The discriminator mean cannot do this job: the noise
// pulls it to the filter centre; the centroid of the power spectrum is not pulled. Steps are 80 % of the centroid, with a dead band of 500 Hz
// because the centroid of a short stretch of FSK is data dependent by a few hundred Hz. Does nothing until the window carries at least a
// quarter of the noise power in excess. Header only.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace dect2 {

class SpectralAfc {
public:
    void configure(double workRate, double searchHz = 9000.0, double maxHz = 20000.0) {
        work_ = workRate; search_ = searchHz; max_ = maxHz; adj_ = 0; afcFft_.reset(); fill_ = 0; blocks_ = 0; settled_ = false; look_ = 0;
    }
    void restart() { fill_ = 0; blocks_ = 0; }          // the stream was interrupted: the averages start again (the correction stays)
    void clearAdjust() { adj_ = 0; blocks_ = 0; settled_ = false; look_ = 0; }
    double adjustHz() const { return adj_; }
    bool settled() const { return settled_; }      // a look has found the signal centred (inside the dead band) since the last clearAdjust / configure
    // open loop: adjustHz() is the offset of the signal from 0 Hz of the stream that is fed (nothing is retuned, the caller shifts its
    // filter instead); the estimate counts as settled when two looks agree inside the dead band
    void setOpenLoop(bool on) { open_ = on; }
    void setDeadBandHz(double hz) { dead_ = hz; }
    void setAverageSec(double s) { sec_ = s; }
public:
    void push(const cf32* x, size_t n) {
        if (!afcFft_) {
            n_ = 128;
            while (n_ < work_ / 250.0 && n_ < 1024) n_ *= 2;
            afcFft_ = std::make_unique<Fft>(n_);
            win_.resize((size_t)n_); buf_.assign((size_t)n_, cf32(0, 0)); avg_.assign((size_t)n_, 0.0);
            for (int i = 0; i < n_; i++) win_[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * 3.14159265358979323846 * (i + 0.5) / n_));
            fill_ = 0;
        }
        const double alpha = std::min(0.5, (double)n_ / (avgSec() * work_));
        for (size_t iv = 0; iv < n; iv++) {
            const cf32 v = x[iv];
            buf_[(size_t)fill_] = v * win_[(size_t)fill_];
            if (++fill_ < n_) continue;
            fill_ = 0;
            afcFft_->forward(buf_.data());
            for (int k = 0; k < n_; k++) {
                const double p = (double)std::norm(buf_[(size_t)k]);
                avg_[(size_t)k] += (blocks_ == 0 ? 1.0 : alpha) * (p - avg_[(size_t)k]);
            }
            if (++blocks_ < (int)std::ceil(avgSec() * work_ / n_)) continue;
            // floor: the quietest quarter of the bins
            tmp_ = avg_;
            std::nth_element(tmp_.begin(), tmp_.begin() + n_ / 4, tmp_.end());
            const double floor = std::max(1e-30, tmp_[(size_t)(n_ / 4)]);
            double sw = 0, swf = 0, excess = 0; int nb = 0;
            for (int k = 0; k < n_; k++) {
                const int kk = k < n_ / 2 ? k : k - n_;
                const double f = kk * work_ / n_;
                if (std::fabs(f) > search_) continue;
                nb++;
                const double e = avg_[(size_t)k] - floor;
                excess += e;
                if (avg_[(size_t)k] < 1.4 * floor) continue;
                sw += e; swf += e * f;
            }
            if (sw > 0 && excess > 0.25 * floor * nb) {
                const double c = swf / sw;
                if (open_) {
                    if (look_ > 0 && std::fabs(c - adj_) <= dead_) { settled_ = true; adj_ = std::max(-max_, std::min(max_, 0.5 * (adj_ + c))); }
                    else { adj_ = std::max(-max_, std::min(max_, c)); settled_ = false; }
                    look_++;
                    blocks_ = 1;     // the next look comes after another averaging period (the averages keep running)
                    continue;
                }
                if (std::fabs(c) <= dead_) settled_ = true;
                if (std::fabs(c) > dead_) {
                    adj_ = std::max(-max_, std::min(max_, adj_ + 0.8 * c));
                    blocks_ = 0;     // the spectrum moved under the averages: start them again
                    continue;
                }
            }
            blocks_ = (int)std::ceil(avgSec() * work_ / n_) - 1;   // keep averaging, look again at the next block
        }
    }
private:
    bool open_ = false;
    int look_ = 0;
    double avgSec() const { return settled_ ? sec_ : 0.3; }   // look quickly until the first look that finds the signal centred
    bool settled_ = false;
    double work_ = 0, search_ = 9000, max_ = 20000, adj_ = 0, dead_ = 500, sec_ = 0.8;
    int n_ = 0, fill_ = 0, blocks_ = 0;
    std::unique_ptr<Fft> afcFft_;
    std::vector<float> win_;
    std::vector<cf32> buf_;
    std::vector<double> avg_, tmp_;

};

} // namespace dect2
