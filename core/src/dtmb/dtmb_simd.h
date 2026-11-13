// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Small vector kernels of the DTMB code: dot products of a real filter with a planar complex signal. NEON where it exists, else plain loops
// with independent accumulators that the compiler can vectorise (float sums are not reassociated by default, so one accumulator would be serial).
#pragma once
#include "dect2/dsp/simd.h"
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
#include <arm_neon.h>
#endif

namespace dect2::dtmb {

// sr = sum_t re[t] h[t], si = sum_t im[t] h[t] for t < n; n is a multiple of 8 or the arrays carry zeros up to the next multiple of 8
DECT2_SIMD_BODY void dotRI(const float* re, const float* im, const float* h, int n, float& sr, float& si) {
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
    float32x4_t a0 = vdupq_n_f32(0), a1 = a0, b0 = a0, b1 = a0;
    for (int t = 0; t < n; t += 8) {
        const float32x4_t h0 = vld1q_f32(h + t), h1 = vld1q_f32(h + t + 4);
        a0 = vfmaq_f32(a0, vld1q_f32(re + t), h0); a1 = vfmaq_f32(a1, vld1q_f32(re + t + 4), h1);
        b0 = vfmaq_f32(b0, vld1q_f32(im + t), h0); b1 = vfmaq_f32(b1, vld1q_f32(im + t + 4), h1);
    }
    sr = vaddvq_f32(vaddq_f32(a0, a1));
    si = vaddvq_f32(vaddq_f32(b0, b1));
#else
    float ar[8] = {0, 0, 0, 0, 0, 0, 0, 0}, ai[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (int t = 0; t < n; t += 8)
        for (int j = 0; j < 8; j++) { ar[j] += re[t + j] * h[t + j]; ai[j] += im[t + j] * h[t + j]; }
    sr = ((ar[0] + ar[4]) + (ar[1] + ar[5])) + ((ar[2] + ar[6]) + (ar[3] + ar[7]));
    si = ((ai[0] + ai[4]) + (ai[1] + ai[5])) + ((ai[2] + ai[6]) + (ai[3] + ai[7]));
#endif
}

#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
// Two dot products at once (two outputs of a polyphase filter): each is exactly dotRI's arithmetic, but the eight accumulator chains are independent,
// so the four-cycle latency of the multiply-adds of one output is hidden behind the other's (one output alone is latency-bound, not load-bound).
inline void dotRI2(const float* reA, const float* imA, const float* hA, const float* reB, const float* imB, const float* hB, int n,
                   float& srA, float& siA, float& srB, float& siB) {
    float32x4_t a0 = vdupq_n_f32(0), a1 = a0, b0 = a0, b1 = a0, c0 = a0, c1 = a0, d0 = a0, d1 = a0;
    for (int t = 0; t < n; t += 8) {
        const float32x4_t h0 = vld1q_f32(hA + t), h1 = vld1q_f32(hA + t + 4), g0 = vld1q_f32(hB + t), g1 = vld1q_f32(hB + t + 4);
        a0 = vfmaq_f32(a0, vld1q_f32(reA + t), h0); a1 = vfmaq_f32(a1, vld1q_f32(reA + t + 4), h1);
        c0 = vfmaq_f32(c0, vld1q_f32(reB + t), g0); c1 = vfmaq_f32(c1, vld1q_f32(reB + t + 4), g1);
        b0 = vfmaq_f32(b0, vld1q_f32(imA + t), h0); b1 = vfmaq_f32(b1, vld1q_f32(imA + t + 4), h1);
        d0 = vfmaq_f32(d0, vld1q_f32(imB + t), g0); d1 = vfmaq_f32(d1, vld1q_f32(imB + t + 4), g1);
    }
    srA = vaddvq_f32(vaddq_f32(a0, a1)); siA = vaddvq_f32(vaddq_f32(b0, b1));
    srB = vaddvq_f32(vaddq_f32(c0, c1)); siB = vaddvq_f32(vaddq_f32(d0, d1));
}
#endif

} // namespace dect2::dtmb
