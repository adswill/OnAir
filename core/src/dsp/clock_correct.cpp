// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/dsp/clock_correct.h"
#include "dect2/common/window.h"
#include <algorithm>
#include <cmath>
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
#include <arm_neon.h>
#endif

namespace dect2 {

static double bessel0(double x) { return dect2::window::besselI0(x, 40, 0.0); }

ClockCorrector::ClockCorrector() {
    // Kaiser-windowed sinc, cut-off at half the rate: the signals it serves fill at most 0.42 of it on each side (a T2 8 MHz channel at
    // 64/7 Msps), so the images of the fractional delay start at 0.58 and the window only has to reach from 0.42 to 0.58.
    constexpr int half = kTaps / 2;
    const double beta = 7.0, i0b = bessel0(beta);
    bank_.assign((size_t)(kPhases + 1) * kTaps, 0.f);
    for (int p = 0; p <= kPhases; p++) {
        const double tau = (double)p / kPhases;
        double t[kTaps], sum = 0;
        for (int k = 0; k < kTaps; k++) {
            const double x = (k - (half - 1)) - tau;
            const double r = x / (half + 0.5);
            const double w = std::fabs(r) < 1 ? bessel0(beta * std::sqrt(1.0 - r * r)) / i0b : 0.0;
            t[k] = (x == 0 ? 1.0 : std::sin(M_PI * x) / (M_PI * x)) * w;
            sum += t[k];
        }
        for (int k = 0; k < kTaps; k++) bank_[(size_t)p * kTaps + k] = (float)(t[k] / sum);
    }
    prime(nullptr, 0);
}

void ClockCorrector::prime(const cf32* hist, size_t n) {
    const size_t need = kTaps / 2;
    const size_t pad = n < need ? need - n : 0;
    re_.assign(pad, 0.f); im_.assign(pad, 0.f);
    for (size_t i = 0; i < n; i++) { re_.push_back(hist[i].real()); im_.push_back(hist[i].imag()); }
    pos_ = (double)re_.size();
}

void ClockCorrector::process(const cf32* in, size_t n, std::vector<cf32>& out) {
    if (!n) return;
    const size_t old = re_.size();
    re_.resize(old + n); im_.resize(old + n);
    for (size_t i = 0; i < n; i++) { re_[old + i] = in[i].real(); im_[old + i] = in[i].imag(); }
    const long avail = (long)re_.size();
    const float* R = re_.data();
    const float* I = im_.data();
    const float* B = bank_.data();
    out.reserve(out.size() + (size_t)((double)n / step_) + 2);
    double pos = pos_;
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
    // Two outputs per pass with NEON: lane j of the first / second register of each sum is ar[j] / ar[4 + j] of the loop below (fused
    // multiply-adds, as the compiler makes of it), and the lanes are added in the same order, so the values are the same bit for bit; the
    // compiler's own vectorisation of that loop was two lanes wide with shuffles. Two independent outputs hide the multiply-add latency.
    {
        auto join = [](float32x4_t a0, float32x4_t a1) {   // ((a0[0]+a1[0]) + (a0[1]+a1[1])) + ((a0[2]+a1[2]) + (a0[3]+a1[3]))
            const float32x4_t s = vaddq_f32(a0, a1);
            return (vgetq_lane_f32(s, 0) + vgetq_lane_f32(s, 1)) + (vgetq_lane_f32(s, 2) + vgetq_lane_f32(s, 3));
        };
        static_assert(kTaps % 8 == 0, "two 4-lane registers per 8 taps");
        for (;;) {
            const long iA = (long)pos, bA = iA - (kTaps / 2 - 1);
            const double posB = pos + step_;
            const long iB = (long)posB, bB = iB - (kTaps / 2 - 1);
            if (bB + kTaps > avail || bA + kTaps > avail) break;
            const float* hA = B + (size_t)((int)((pos - (double)iA) * kPhases + 0.5)) * kTaps;
            const float* hB = B + (size_t)((int)((posB - (double)iB) * kPhases + 0.5)) * kTaps;
            float32x4_t r0 = vdupq_n_f32(0), r1 = r0, m0 = r0, m1 = r0, s0 = r0, s1 = r0, n0 = r0, n1 = r0;
            for (int t = 0; t < kTaps; t += 8) {
                const float32x4_t h0 = vld1q_f32(hA + t), h1 = vld1q_f32(hA + t + 4), g0 = vld1q_f32(hB + t), g1 = vld1q_f32(hB + t + 4);
                r0 = vfmaq_f32(r0, vld1q_f32(R + bA + t), h0); r1 = vfmaq_f32(r1, vld1q_f32(R + bA + t + 4), h1);
                m0 = vfmaq_f32(m0, vld1q_f32(I + bA + t), h0); m1 = vfmaq_f32(m1, vld1q_f32(I + bA + t + 4), h1);
                s0 = vfmaq_f32(s0, vld1q_f32(R + bB + t), g0); s1 = vfmaq_f32(s1, vld1q_f32(R + bB + t + 4), g1);
                n0 = vfmaq_f32(n0, vld1q_f32(I + bB + t), g0); n1 = vfmaq_f32(n1, vld1q_f32(I + bB + t + 4), g1);
            }
            out.emplace_back(join(r0, r1), join(m0, m1));
            out.emplace_back(join(s0, s1), join(n0, n1));
            pos = posB + step_;
        }
    }
#endif
    for (;;) {
        const long i0 = (long)pos;
        const long base = i0 - (kTaps / 2 - 1);
        if (base + kTaps > avail) break;
        const int p = (int)((pos - (double)i0) * kPhases + 0.5);
        const float* h = B + (size_t)p * kTaps;
        const float* r = R + base;
        const float* m = I + base;
        float ar[8] = {}, ai[8] = {};   // eight lanes, so that the compiler can use vector registers without reordering a float sum
        for (int t = 0; t < kTaps; t += 8)
            for (int j = 0; j < 8; j++) { ar[j] += r[t + j] * h[t + j]; ai[j] += m[t + j] * h[t + j]; }
        out.emplace_back(((ar[0] + ar[4]) + (ar[1] + ar[5])) + ((ar[2] + ar[6]) + (ar[3] + ar[7])),
                         ((ai[0] + ai[4]) + (ai[1] + ai[5])) + ((ai[2] + ai[6]) + (ai[3] + ai[7])));
        pos += step_;
    }
    const long keepFrom = (long)pos - (kTaps / 2 - 1);
    if (keepFrom > 4096) {
        re_.erase(re_.begin(), re_.begin() + keepFrom);
        im_.erase(im_.begin(), im_.begin() + keepFrom);
        pos -= (double)keepFrom;
    }
    pos_ = pos;
}

} // namespace dect2
