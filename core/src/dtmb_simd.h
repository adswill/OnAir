// Small vector kernels of the DTMB code: dot products of a real filter with a planar complex signal. NEON where it exists, else plain loops
// with independent accumulators that the compiler can vectorise (float sums are not reassociated by default, so one accumulator would be serial).
#pragma once
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
#include <arm_neon.h>
#endif

namespace dect2::dtmb {

// sr = sum_t re[t] h[t], si = sum_t im[t] h[t] for t < n; n is a multiple of 8 or the arrays carry zeros up to the next multiple of 8
inline void dotRI(const float* re, const float* im, const float* h, int n, float& sr, float& si) {
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

} // namespace dect2::dtmb
