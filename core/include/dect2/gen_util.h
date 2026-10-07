// Small helpers shared by the test signal generators of ATSC 1.0, ISDB-T and DVB-T: a table of Gaussian noise read from pseudo
// random positions (std::normal_distribution with a Mersenne twister costs more than all the rest of a cheap generator), and a
// dot product written with independent partial sums so that every compiler vectorises it.
#pragma once
#include <complex>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>
#if (defined(__ARM_NEON) || defined(__aarch64__)) && !defined(DECT2_NO_SIMD)
#include <arm_neon.h>
#elif defined(__SSE2__) && !defined(DECT2_NO_SIMD)
#include <emmintrin.h>
#endif

namespace dect2 {
namespace genutil {

// white Gaussian noise, unit variance per real component, one table shared by every generator
inline const std::vector<std::complex<float>>& noiseTable() {
    static const std::vector<std::complex<float>> t = [] {
        std::vector<std::complex<float>> v((size_t)1 << 19);
        std::mt19937 rng(20240611);
        std::normal_distribution<float> nd(0.f, 1.f);
        for (auto& x : v) { const float re = nd(rng); x = std::complex<float>(re, nd(rng)); }
        return v;
    }();
    return t;
}

// Reads the table from a new pseudo random start every 4096 values.
class NoiseSource {
public:
    explicit NoiseSource(uint32_t seed = 1) : state_(seed * 2654435761u + 0x9E3779B9u) { if (!state_) state_ = 1; }
    // v[i] += sigma * noise, for n values
    void add(std::complex<float>* v, size_t n, float sigma) {
        const auto& t = noiseTable();
        const size_t mask = t.size() - 1;
        size_t i = 0;
        while (i < n) {
            if (left_ == 0) {
                state_ ^= state_ << 13; state_ ^= state_ >> 17; state_ ^= state_ << 5;
                pos_ = (size_t)(state_ & mask);
                left_ = 4096;
            }
            size_t run = n - i < left_ ? n - i : left_;
            if (pos_ + run > t.size()) run = t.size() - pos_;
            const std::complex<float>* src = &t[pos_];
            float* d = reinterpret_cast<float*>(v + i);
            const float* s = reinterpret_cast<const float*>(src);
            for (size_t k = 0; k < 2 * run; k++) d[k] += sigma * s[k];
            i += run; pos_ += run; left_ -= run;
            if (pos_ >= t.size()) pos_ = 0;
        }
    }
    // one value
    std::complex<float> next(float sigma) {
        std::complex<float> v(0.f, 0.f);
        add(&v, 1, sigma);
        return v;
    }
private:
    uint32_t state_;
    size_t pos_ = 0, left_ = 0;
};

// sum_i h[i] * a[i] and sum_i h[i] * b[i]. NEON and SSE2 where available (the same pattern as core/src/dvbs_simd.h), plain code
// with independent partial sums elsewhere.
inline void dot2(const float* h, const float* a, const float* b, int n, float* out) {
    int i = 0;
#if (defined(__ARM_NEON) || defined(__aarch64__)) && !defined(DECT2_NO_SIMD)
    float32x4_t s0 = vdupq_n_f32(0), s1 = s0, t0 = s0, t1 = s0;
    for (; i + 8 <= n; i += 8) {
        const float32x4_t ha = vld1q_f32(h + i), hb = vld1q_f32(h + i + 4);
        s0 = vfmaq_f32(s0, ha, vld1q_f32(a + i)); t0 = vfmaq_f32(t0, hb, vld1q_f32(a + i + 4));
        s1 = vfmaq_f32(s1, ha, vld1q_f32(b + i)); t1 = vfmaq_f32(t1, hb, vld1q_f32(b + i + 4));
    }
    float ra = vaddvq_f32(vaddq_f32(s0, t0)), rb = vaddvq_f32(vaddq_f32(s1, t1));
#elif defined(__SSE2__) && !defined(DECT2_NO_SIMD)
    __m128 s0 = _mm_setzero_ps(), s1 = s0, t0 = s0, t1 = s0;
    for (; i + 8 <= n; i += 8) {
        const __m128 ha = _mm_loadu_ps(h + i), hb = _mm_loadu_ps(h + i + 4);
        s0 = _mm_add_ps(s0, _mm_mul_ps(ha, _mm_loadu_ps(a + i))); t0 = _mm_add_ps(t0, _mm_mul_ps(hb, _mm_loadu_ps(a + i + 4)));
        s1 = _mm_add_ps(s1, _mm_mul_ps(ha, _mm_loadu_ps(b + i))); t1 = _mm_add_ps(t1, _mm_mul_ps(hb, _mm_loadu_ps(b + i + 4)));
    }
    float fa[4], fb[4];
    _mm_storeu_ps(fa, _mm_add_ps(s0, t0)); _mm_storeu_ps(fb, _mm_add_ps(s1, t1));
    float ra = (fa[0] + fa[2]) + (fa[1] + fa[3]), rb = (fb[0] + fb[2]) + (fb[1] + fb[3]);
#else
    float sa[8] = {}, sb[8] = {};
    for (; i + 8 <= n; i += 8)
        for (int l = 0; l < 8; l++) { sa[l] += h[i + l] * a[i + l]; sb[l] += h[i + l] * b[i + l]; }
    float ra = ((sa[0] + sa[4]) + (sa[1] + sa[5])) + ((sa[2] + sa[6]) + (sa[3] + sa[7]));
    float rb = ((sb[0] + sb[4]) + (sb[1] + sb[5])) + ((sb[2] + sb[6]) + (sb[3] + sb[7]));
#endif
    for (; i < n; i++) { ra += h[i] * a[i]; rb += h[i] * b[i]; }
    out[0] = ra; out[1] = rb;
}

} // namespace genutil
} // namespace dect2
