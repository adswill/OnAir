// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DTMB front end (see dtmb_front.h).
#include "dect2/dtmb/dtmb_front.h"
#include "dect2/dtmb/dtmb_defs.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "dtmb_simd.h"
#ifdef DECT2_X86_DISPATCH
#include <immintrin.h>
#endif
#include "dect2/common/window.h"

namespace dect2::dtmb {

namespace {
constexpr int kSpan = 20;      // symbols on each side of the pulse
constexpr double kBeta = 5.0;  // Kaiser window of the truncated pulse
double besselI0(double x) { return dect2::window::besselI0(x, 50, 0.0); }

}

bool SrrcResampler::configure(double inRate, double symRate) {
    if (inRate < symRate * 1.0516) return false;   // 7.95 Msps for the 8 MHz channel: the roll-off of 0.05 has to fit
    inRate_ = inRate;
    ratio_ = inRate / symRate;
    half_ = (int)std::ceil(kSpan * ratio_);
    taps_ = 2 * half_;
    stride_ = (taps_ + 7) & ~7;
    bank_.assign((size_t)(phases_ + 1) * (size_t)stride_, 0.f);
    const double i0b = besselI0(kBeta);
    for (int p = 0; p <= phases_; p++) {
        const double f = (double)p / phases_;
        for (int k = 0; k < taps_; k++) {
            const double d = (double)(k - half_ + 1) - f;        // input samples from the output instant
            const double t = d / ratio_;                          // symbol periods
            const double u = t / kSpan;
            const double w = std::fabs(u) < 1.0 ? besselI0(kBeta * std::sqrt(1.0 - u * u)) / i0b : 0.0;
            bank_[(size_t)p * (size_t)stride_ + (size_t)k] = (float)(srrcPulse(t) * w / ratio_);
        }
    }
    reset();
    return true;
}

void SrrcResampler::reset() {
    re_.assign((size_t)half_ + 2, 0.f);   // silence in front of the first sample
    im_.assign(re_.size(), 0.f);
    fill_ = re_.size();
    pos_ = (int64_t)std::llround((double)re_.size() * kOne);
    spacing_ = 0;
    step_ = (int64_t)std::llround(ratio_ * kOne);
    dc_re_ = dc_im_ = 0;
    dcCount_ = 0;
}

void SrrcResampler::push(const cf32* in, size_t n) {
    if (!n || taps_ == 0) return;
    // DC offset of the radio: a slow average (time constant 25 ms), a plain average at first so that it settles quickly; updated per block
    double sr = 0, si = 0;
    for (size_t i = 0; i < n; i++) { sr += in[i].real(); si += in[i].imag(); }
    const double alpha = std::max((double)n / (inRate_ * 0.025), (double)n / ((double)dcCount_ + (double)n));
    dc_re_ += std::min(1.0, alpha) * (sr / (double)n - dc_re_);
    dc_im_ += std::min(1.0, alpha) * (si / (double)n - dc_im_);
    dcCount_ += n;
    const float dr = dcOn_ ? (float)dc_re_ : 0.f, di = dcOn_ ? (float)dc_im_ : 0.f;
    if (fill_ + n + (size_t)stride_ + 16 > re_.size()) { re_.resize(fill_ + n + (size_t)stride_ + 16 + 65536); im_.resize(re_.size()); }
    float* r = &re_[fill_];
    float* m = &im_[fill_];
    for (size_t i = 0; i < n; i++) { r[i] = in[i].real() - dr; m[i] = in[i].imag() - di; }
    fill_ += n;
}

size_t SrrcResampler::available() const {
    // outputs with base + stride <= fill
    const int64_t lastPos = (int64_t)(fill_ - (size_t)stride_ + (size_t)(half_ - 1)) * (int64_t)kOne;
    if (lastPos < pos_) return 0;
    return (size_t)((lastPos - pos_) / step_) + 1;
}

namespace {
// The polyphase filter over n outputs; compiled twice on x86-64 (baseline and AVX2 + FMA, see dect2/dsp/simd.h)
DECT2_SIMD_BODY void filterLoopBaseBody(const float* re, const float* im, const float* bank, int stride, int half, int64_t& pos, int64_t step, size_t n, cf32* o) {
    const int shift = 32 - 9;   // 512 phases
    size_t k = 0;
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
    for (; k + 2 <= n; k += 2) {   // two outputs per pass (independent, so the second does not wait for the first); the positions are exact integers
        long base[2]; const float* h[2];
        for (int u = 0; u < 2; u++) {
            int64_t i0 = pos >> 32;
            uint32_t ph = (uint32_t)(((uint64_t)(pos & 0xFFFFFFFFll) + (1ull << (shift - 1))) >> shift);
            if (ph >= 512) { ph = 0; i0++; }
            base[u] = (long)i0 - (half - 1);
            h[u] = bank + (size_t)ph * (size_t)stride;
            pos += step;
        }
        float vr[2], vi[2];
        dotRI2(re + base[0], im + base[0], h[0], re + base[1], im + base[1], h[1], stride, vr[0], vi[0], vr[1], vi[1]);
        o[k] = cf32(vr[0], vi[0]); o[k + 1] = cf32(vr[1], vi[1]);
    }
#endif
    for (; k < n; k++) {
        int64_t i0 = pos >> 32;
        // nearest of the 512 phases; a carry moves to the next input sample
        uint32_t ph = (uint32_t)(((uint64_t)(pos & 0xFFFFFFFFll) + (1ull << (shift - 1))) >> shift);
        if (ph >= 512) { ph = 0; i0++; }
        const long base = (long)i0 - (half - 1);
        float vr, vi;
        dotRI(re + base, im + base, bank + (size_t)ph * (size_t)stride, stride, vr, vi);
        o[k] = cf32(vr, vi);
        pos += step;
    }
}
void filterLoopBase(const float* re, const float* im, const float* bank, int stride, int half, int64_t& pos, int64_t step, size_t n, cf32* o);
#ifdef DECT2_X86_DISPATCH
// AVX2 + FMA, four outputs per pass (eight independent accumulator chains: one output alone is bound by the multiply-add latency)
__attribute__((target("avx2,fma")))
void filterLoop4(const float* re, const float* im, const float* bank, int stride, int half, int64_t& pos, int64_t step, size_t n, cf32* o) {
    const int shift = 32 - 9;
    size_t k = 0;
    for (; k + 4 <= n; k += 4) {
        long base[4]; const float* h[4];
        for (int u = 0; u < 4; u++) {
            int64_t i0 = pos >> 32;
            uint32_t ph = (uint32_t)(((uint64_t)(pos & 0xFFFFFFFFll) + (1ull << (shift - 1))) >> shift);
            if (ph >= 512) { ph = 0; i0++; }
            base[u] = (long)i0 - (half - 1);
            h[u] = bank + (size_t)ph * (size_t)stride;
            pos += step;
        }
        __m256 ar[4], ai[4];
        for (int u = 0; u < 4; u++) ar[u] = ai[u] = _mm256_setzero_ps();
        for (int t = 0; t < stride; t += 8)
            for (int u = 0; u < 4; u++) {
                const __m256 hv = _mm256_loadu_ps(h[u] + t);
                ar[u] = _mm256_fmadd_ps(_mm256_loadu_ps(re + base[u] + t), hv, ar[u]);
                ai[u] = _mm256_fmadd_ps(_mm256_loadu_ps(im + base[u] + t), hv, ai[u]);
            }
        for (int u = 0; u < 4; u++) {
            __m128 a = _mm_add_ps(_mm256_castps256_ps128(ar[u]), _mm256_extractf128_ps(ar[u], 1)), b = _mm_add_ps(_mm256_castps256_ps128(ai[u]), _mm256_extractf128_ps(ai[u], 1));
            a = _mm_hadd_ps(a, b); a = _mm_hadd_ps(a, a);   // re, im
            o[k + u] = cf32(_mm_cvtss_f32(a), _mm_cvtss_f32(_mm_shuffle_ps(a, a, 1)));
        }
    }
    if (k < n) filterLoopBase(re, im, bank, stride, half, pos, step, n - k, o + k);
}
#endif
DECT2_AVX2_DISPATCH(void, filterLoopBase, (const float* re, const float* im, const float* bank, int stride, int half, int64_t& pos, int64_t step, size_t n, cf32* o), (re, im, bank, stride, half, pos, step, n, o))
void filterLoop(const float* re, const float* im, const float* bank, int stride, int half, int64_t& pos, int64_t step, size_t n, cf32* o) {
#ifdef DECT2_X86_DISPATCH
    if (cpuHasAvx2()) { filterLoop4(re, im, bank, stride, half, pos, step, n, o); return; }
#endif
    filterLoopBase(re, im, bank, stride, half, pos, step, n, o);
}
}

size_t SrrcResampler::pull(std::vector<cf32>& out, size_t maxOut) {
    if (taps_ == 0) return 0;
    size_t n = std::min(available(), maxOut);
    const size_t at = out.size();
    out.resize(at + n);
    cf32* o = out.data() + at;
    filterLoop(re_.data(), im_.data(), bank_.data(), stride_, half_, pos_, step_, n, o);
    // drop what is no longer needed
    const long keepFrom = (long)(pos_ >> 32) - (half_ - 1) - 1 - kHistory;
    if (keepFrom > 16384) {
        const size_t d = (size_t)keepFrom;
        std::memmove(re_.data(), re_.data() + d, (fill_ - d) * sizeof(float));
        std::memmove(im_.data(), im_.data() + d, (fill_ - d) * sizeof(float));
        fill_ -= d;
        pos_ -= (int64_t)d << 32;
    }
    return n;
}

} // namespace dect2::dtmb
