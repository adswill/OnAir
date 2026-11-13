// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/engine/channelizer.h"
#include "dect2/common/diag.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace dect2 {

bool ChannelFilter::configure(double inRate, double offsetHz, double outRate) {
    if (inRate <= 0 || outRate <= 0) {
        DIAG_DEBUG(813231, "A channel filter was asked for the input rate %.0f Hz and the output rate %.0f Hz, and a rate must be above zero, so it is not set up.", inRate, outRate);
        return false;
    }
    inRate_ = inRate; outRate_ = outRate; shift_ = offsetHz;
    if (!rs_.configure(inRate, outRate, 8)) {
        DIAG_DEBUG(624691, "The channel filter resampler refused the rates %.0f Hz to %.0f Hz, so the channel is not set up.", inRate, outRate);
        return false;
    }
    const double w = -2 * M_PI * offsetHz / inRate;
    tre_.resize(L); tim_.resize(L);
    for (size_t k = 0; k < L; k++) { tre_[k] = (float)std::cos(w * (double)k); tim_[k] = (float)std::sin(w * (double)k); }
    stepRe_ = std::cos(w * (double)L); stepIm_ = std::sin(w * (double)L);
    reset();
    return true;
}

void ChannelFilter::reset() { rs_.reset(); pos_ = 0; phRe_ = 1; phIm_ = 0; }

void ChannelFilter::process(const cf32* x, size_t n, std::vector<cf32>& out) {
    if (!n) return;
    if (mix_.size() < n) mix_.resize(n);
    if (shift_ == 0) { std::copy(x, x + n, mix_.begin()); }
    else {
        size_t i = 0;
        while (i < n) {
            const size_t seg = std::min(n - i, L - pos_);
            const float pr = (float)phRe_, pi = (float)phIm_;
            const float* tr = tre_.data() + pos_;
            const float* ti = tim_.data() + pos_;
            const float* in = reinterpret_cast<const float*>(x + i);
            float* o = reinterpret_cast<float*>(mix_.data() + i);
            for (size_t k = 0; k < seg; k++) {
                const float lr = tr[k] * pr - ti[k] * pi, li = tr[k] * pi + ti[k] * pr;
                const float xr = in[2 * k], xi = in[2 * k + 1];
                o[2 * k] = xr * lr - xi * li;
                o[2 * k + 1] = xr * li + xi * lr;
            }
            i += seg; pos_ += seg;
            if (pos_ == L) {
                pos_ = 0;
                const double nr = phRe_ * stepRe_ - phIm_ * stepIm_, ni = phRe_ * stepIm_ + phIm_ * stepRe_;
                const double m = std::sqrt(nr * nr + ni * ni);
                phRe_ = nr / m; phIm_ = ni / m;
            }
        }
    }
    rs_.process(mix_.data(), n, out);
}

double tvChannelRate(double widthMhz, double spanRate) { return std::min(spanRate, widthMhz * 1.25e6); }

std::vector<ChannelSlotPos> spanSlots(double spanRate, double widthHz, double gridOffsetHz, double edgeFrac) {
    std::vector<ChannelSlotPos> v;
    if (spanRate <= 0 || widthHz <= 0) return v;
    const double lim = edgeFrac * spanRate;
    const int kMax = (int)std::ceil(spanRate / widthHz) + 1;
    for (int k = -kMax; k <= kMax; k++) {
        const double c = gridOffsetHz + k * widthHz;
        if (std::fabs(c) + widthHz * 0.5 <= lim + 1.0) v.push_back(ChannelSlotPos{k, c});
    }
    return v;
}

void SpanScanner::configure(double spanRate) {
    rate_ = spanRate;
    win_.resize(kN);
    for (int i = 0; i < kN; i++) win_[i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / kN));
    reset();
}

void SpanScanner::reset() { acc_.assign(kN, 0.f); fill_ = 0; nFrames_ = 0; blocks_ = 0; dirty_ = true; blk_.assign(kN, cf32(0, 0)); }

void SpanScanner::feed(const cf32* x, size_t n) {
    static thread_local Fft fft(kN);
    static thread_local std::vector<cf32> buf;
    size_t i = 0;
    while (i < n) {
        // only one block in eight goes through the FFT: the spectrum needs few frames, not every sample
        const bool take = (blocks_ & 7) == 0;
        const size_t m = std::min(n - i, (size_t)kN - fill_);
        if (take) std::copy(x + i, x + i + m, blk_.begin() + (long)fill_);
        fill_ += m; i += m;
        if (fill_ == (size_t)kN) {
            fill_ = 0; blocks_++;
            if (take) {
                buf.resize(kN);
                for (int k = 0; k < kN; k++) buf[k] = blk_[k] * win_[k];
                fft.forward(buf.data());
                // exponential average (the first frames weigh equally)
                const float a = nFrames_ < 16 ? 1.f / (float)(nFrames_ + 1) : 1.f / 16.f;
                for (int k = 0; k < kN; k++) {
                    const int j = (k + kN / 2) % kN;   // DC in the middle
                    const float p = std::norm(buf[j]) / ((float)kN * (float)kN * 0.375f);
                    acc_[k] += a * (p - acc_[k]);
                }
                nFrames_++; dirty_ = true;
            }
        }
    }
}

void SpanScanner::refresh() const {
    if (!dirty_) return;
    db_.resize(kN);
    for (int k = 0; k < kN; k++) db_[k] = 10.f * std::log10(std::max(acc_[k], 1e-20f));
    dirty_ = false;
}

double SpanScanner::floorDb(double edgeFrac) const {
    if (!nFrames_) return -200;
    const int win = std::max(4, (int)std::lround(250e3 / (rate_ / kN)));
    const int lo = (int)std::lround(kN / 2 - edgeFrac * kN), hi = (int)std::lround(kN / 2 + edgeFrac * kN);
    if (hi - lo < win + 2 || lo < 0 || hi > kN) return -200;
    // the quietest window by mean power (linear), a running sum
    double s = 0;
    for (int k = lo; k < lo + win; k++) s += acc_[k];
    double best = s;
    for (int k = lo + win; k < hi; k++) { s += acc_[k] - acc_[k - win]; best = std::min(best, s); }
    return 10.0 * std::log10(std::max(best / win, 1e-20));
}

std::vector<SlotPower> SpanScanner::measure(const std::vector<ChannelSlotPos>& slots, double widthHz, double thresholdDb) const {
    std::vector<SlotPower> out;
    if (!nFrames_) return out;
    refresh();
    const double fl = floorDb();
    const double bin = rate_ / kN;
    for (const ChannelSlotPos& s : slots) {
        SlotPower p; p.pos = s;
        const double half = widthHz * 0.45;
        const int a = std::max(0, (int)std::lround(kN / 2 + (s.offsetHz - half) / bin)), b = std::min(kN - 1, (int)std::lround(kN / 2 + (s.offsetHz + half) / bin));
        if (b > a) {
            std::vector<float> v(db_.begin() + a, db_.begin() + b + 1);
            std::nth_element(v.begin(), v.begin() + (long)v.size() / 2, v.end());
            p.levelDb = v[v.size() / 2];
        }
        p.aboveDb = p.levelDb - fl;
        p.occupied = p.aboveDb >= thresholdDb;
        out.push_back(p);
    }
    return out;
}

} // namespace dect2
