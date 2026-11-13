// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode A / C reply detector (see modeac_dsp.h).
#include "dect2/modeac/modeac_dsp.h"
#include "dect2/modeac/modeac_proto.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

using namespace modeac;

void ModeAcDemod::configure(double rate) {
    rate_ = rate;
    spus_ = rate * 1e-6;
    reset();
}

void ModeAcDemod::reset() {
    pw_.clear(); cum_.assign(1, 0.0);
    base_ = (int64_t)samples_;
    scanPos_ = (double)base_;
    dcInit_ = false;
    blockSum_ = 0; blockCount_ = 0; blockN_ = 0; blockPos_ = 0; n0_ = 0; noiseReady_ = false;
}

double ModeAcDemod::noiseDbfs() const { return n0_ > 0 ? 10.0 * std::log10((double)n0_) : -120.0; }

void ModeAcDemod::updateNoise(float p) {
    blockSum_ += p;
    if (++blockCount_ < 512) return;
    blocks_[blockPos_] = (float)(blockSum_ / blockCount_);
    blockPos_ = (blockPos_ + 1) % 24; blockN_ = std::min(blockN_ + 1, 24);
    blockSum_ = 0; blockCount_ = 0;
    if (blockN_ < 8) return;
    float v[24];
    std::copy(blocks_, blocks_ + blockN_, v);
    std::nth_element(v, v + blockN_ / 4, v + blockN_);   // the lower quartile: Mode S traffic and replies lift the others
    n0_ = std::max(v[blockN_ / 4], 1e-12f);
    noiseReady_ = true;
}

void ModeAcDemod::feed(const cf32* x, size_t n) {
    if (!ready() || n == 0) return;
    pw_.reserve(pw_.size() + n);
    cum_.reserve(cum_.size() + n);
    for (size_t i = 0; i < n; i++) {
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        if (!dcInit_) { dcRe_ = re; dcIm_ = im; dcInit_ = true; }
        dcRe_ += (re - dcRe_) * (1.f / 4096.f); dcIm_ += (im - dcIm_) * (1.f / 4096.f);
        const float a = re - dcRe_, b = im - dcIm_;
        const float p = a * a + b * b;
        pw_.push_back(p);
        cum_.push_back(cum_.back() + p);
        updateNoise(p);
    }
    samples_ += n;
    scan();
    compact();
}

void ModeAcDemod::compact() {
    const double keepFrom = scanPos_ - 40.0 * spus_ - 4;   // the refinement looks a little before the scan position
    const int64_t drop = (int64_t)std::floor(keepFrom) - base_;
    if (drop < 65536) return;
    const double c0 = cum_[(size_t)drop];
    pw_.erase(pw_.begin(), pw_.begin() + drop);
    cum_.erase(cum_.begin(), cum_.begin() + drop);
    for (double& c : cum_) c -= c0;
    base_ += drop;
}

double ModeAcDemod::win(double x, double w) const {
    auto C = [&](double pos) {
        double l = pos - (double)base_;
        if (l <= 0) return cum_.front();
        const size_t i = (size_t)l;
        if (i >= pw_.size()) return cum_.back();
        return cum_[i] + (l - (double)i) * pw_[i];
    };
    return (C(x + w) - C(x)) / w;
}

void ModeAcDemod::scan() {
    if (!noiseReady_) { scanPos_ = std::max(scanPos_, (double)(base_ + (int64_t)pw_.size()) - (kLagUs + 30.0) * spus_); return; }
    const double endLimit = (double)(base_ + (int64_t)pw_.size()) - (kLagUs + 30.0) * spus_;
    const double w = 0.5 * spus_, step = 0.125 * spus_, d2 = 20.3 * spus_;
    while (scanPos_ < endLimit) {
        const double thr = kPulse_ * n0_;
        const double s = scanPos_;
        if (win(s, w) < thr || win(s + d2, w) < thr) { scanPos_ += step; continue; }
        candidates_++;
        // timing: the offset that makes the weaker of the two framing pulses strongest
        double best = -1, sBest = s;
        for (int k = -8; k <= 8; k++) {
            const double sx = s + k * 0.05 * spus_;
            const double sc = std::min(win(sx, w), win(sx + d2, w));
            if (sc > best) { best = sc; sBest = sx; }
        }
        Raw r;
        if (testAt(sBest, r)) {
            accepted_++;
            if (sink_) sink_(r);
            scanPos_ = sBest + 21.0 * spus_;
        } else {
            scanPos_ = sBest + 0.6 * spus_;
        }
    }
}

bool ModeAcDemod::testAt(double s0, Raw& out) {
    const double w = 0.5 * spus_;
    const double off = -0.025 * spus_;          // the window sits on the pulse, 0.025 us early on both sides
    double e[18] = {};
    for (int k = 0; k <= 14; k++) e[k] = win(s0 + off + k * kSlotUs * spus_, w);
    e[17] = win(s0 + off + 17 * kSlotUs * spus_, w);
    const double L = std::min(e[0], e[14]);
    if (L < kPulse_ * n0_ || std::max(e[0], e[14]) > 4.0 * L) { rejShape_++; return false; }   // framing pulses of one transmitter
    const double on = 0.4 * L;
    if (e[7] > on) { rejShape_++; return false; }                     // X is empty
    if (e[15] > on * 0.8 || e[16] > on * 0.8) { rejShape_++; return false; }     // nothing between F2 and SPI
    if (win(s0 + off - kSlotUs * spus_, w) > on) { rejShape_++; return false; }  // and nothing right before F1
    // little energy between the pulses
    double gmax = 0, gsum = 0;
    for (int k = 0; k < 14; k++) {
        const double g = win(s0 + (k * kSlotUs + 0.62) * spus_, 0.7 * spus_);
        gmax = std::max(gmax, g); gsum += g;
    }
    if (gmax > 0.45 * L || gsum / 14.0 > 0.18 * L) { rejShape_++; return false; }
    uint16_t bits = 0; double psum = 0; int pn = 0;
    for (int s = 1; s <= 13; s++) if (e[s] > on) { bits = setSlot(bits, s, true); psum += e[s]; pn++; }
    psum += e[0] + e[14]; pn += 2;
    const double t0 = (s0 / rate_), t1 = t0 + (kReplyUs + 1.0) * 1e-6;
    if (mask_ && mask_(t0 - 1.5e-6, t1)) { masked_++; return false; }
    out.timeSec = t0;
    out.bits = bits;
    out.spi = e[17] > on;
    const double lvl = psum / pn;
    out.levelDbfs = (float)(10.0 * std::log10(std::max(lvl, 1e-12)));
    out.snrDb = (float)(10.0 * std::log10(std::max(lvl / n0_, 1e-6)));
    return true;
}

} // namespace dect2
