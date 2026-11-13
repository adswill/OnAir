// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/dsp/resampler.h"
#include "dect2/common/diag.h"
#include "dect2/common/window.h"
#include "dect2/dsp/dsp_compat.h"
#include "dect2/dsp/simd.h"
#if defined(DECT2_X86_DISPATCH)
#include <immintrin.h>
#endif
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
#include <arm_neon.h>
#endif

namespace dect2 {

static double bessel0(double x) { return dect2::window::besselI0(x, 40, 0.0); }

bool RationalResampler::configure(double inRate, double outRate, double* relError, double occupiedHz) {
    double target = outRate / inRate;
    int bestL = 0, bestM = 0;
    double bestErr = 1e9;
    // up to 1024 phases: the radios' own rates need it to be exact (2.5 -> 2.048 Msps for DAB on an Airspy R2 is 512/625; with at most 128
    // the nearest fraction was 100 ppm off, which cost DAB 13 dB of SNR). One phase is 2*H taps, so 1024 phases are 64k floats.
    for (int L = 1; L <= 1024; L++) {
        int M = (int)std::lround(L / target);
        if (M < 1) continue;
        double err = std::fabs((double)L / M - target) / target;
        if (err < bestErr - 1e-12) { bestErr = err; bestL = L; bestM = M; }
        if (err < 2e-8) break;
    }
    if (relError) *relError = bestErr;
    if (bestErr > 1e-4) {
        DIAG_DEBUG(714685, "No rational resampler with up to 1024 phases comes within 100 ppm of the ratio %.6f (%.3f Hz in, %.3f Hz out), so the resampler stays unconfigured.", target, inRate, outRate);
        return false;
    }
    int g = std::gcd(bestL, bestM);
    L_ = bestL / g; M_ = bestM / g;
    // kernel: lowpass at the lower of the two Nyquist rates, normalised to the input rate
    double fc = 0.5 * std::min(1.0, target) * 0.985;
    H_ = target < 0.99 ? 32 : 16;
    double beta = 7.0;
    if (occupiedHz > 0 && target < 0.99 && occupiedHz < outRate) {
        // The signal only fills occupiedHz of the output band, so only what folds into it has to be stopped: the transition band can run from
        // the signal edge to where the first alias reaches the signal edge, a much wider one than the generic design assumes. Kaiser's estimate
        // of the length for 65 dB: far fewer taps per output sample (the cost of the resampler is the taps), with the same alias rejection.
        const double fp = 0.5 * occupiedHz, fs = outRate - fp;
        const double dw = (fs - fp) / inRate;
        const double atten = 65.0;
        const int taps = (int)std::ceil((atten - 7.95) / (2.285 * 2 * M_PI * dw)) + 1;
        H_ = std::max(8, std::min(H_, ((taps + 1) / 2 + 3) / 4 * 4));   // a whole number of 8 tap blocks: desamp finishes a remainder with scalar code
        fc = 0.5 * (fp + fs) / inRate;
        beta = 0.1102 * (atten - 8.7);
    }
    T_ = 2 * H_;
    taps_.assign(L_, std::vector<float>(T_));
    base_.assign(L_, 0);
    nOfPhase_.assign(L_, 0);
    // n_p such that (n_p * M) mod L = p
    std::vector<int> nOf(L_);
    for (int n = 0; n < L_; n++) nOf[(int)(((long long)n * M_) % L_)] = n;
    for (int p = 0; p < L_; p++) {
        int np = nOf[p];
        long long num = (long long)np * M_;
        base_[p] = (int)(num / L_);
        nOfPhase_[p] = np;
        double frac = (double)(num % L_) / L_;
        double sum = 0;
        std::vector<double> h(T_);
        for (int t = 0; t < T_; t++) {
            double x = (t - (H_ - 1)) - frac; // tap k = t-(H-1) sits at distance k - frac from the output instant
            double a = 2 * fc * x;
            double sinc = std::fabs(a) < 1e-12 ? 1.0 : std::sin(M_PI * a) / (M_PI * a);
            double r = x / H_;
            double w = std::fabs(r) >= 1 ? 0 : bessel0(beta * std::sqrt(1 - r * r)) / bessel0(beta);
            h[t] = 2 * fc * sinc * w;
            sum += h[t];
        }
        for (int t = 0; t < T_; t++) taps_[p][t] = (float)(h[t] / sum);
    }
    // the interleaved path: NEON with the portable desamp (Accelerate's sums in an order that depends on the length: macOS keeps its
    // vDSP_desamp, which is about as fast), a whole number of 8-tap blocks, a stride of at least 2
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD) && !(defined(__APPLE__) && !defined(DECT2_PORTABLE))
    interOk_ = T_ % 8 == 0 && M_ >= 2;
#else
    interOk_ = false;
#endif
#if defined(DECT2_X86_DISPATCH)
    // x86-64 with AVX2: the interleaved path too, for a caller that asks for it (allowFastX86: its sums are in a different order from
    // desamp's, so the other modes keep the split path and their exact output)
    if (fastX86_ && cpuHasAvx2() && T_ % 8 == 0 && M_ >= 2) interOk_ = true;
#endif
    inter_ = interOk_ && allowInter_;
    if (interOk_) {
#if defined(DECT2_X86_DISPATCH)
        constexpr int kDup = 2;   // each tap twice (I, Q)
#else
        constexpr int kDup = 1;
#endif
        tapsD_.assign((size_t)L_ * T_ * kDup, 0.f);
        startOf_.assign(L_, 0);
        for (int p = 0; p < L_; p++) {
            const int ph = nOfPhase_[p];
            startOf_[ph] = base_[p];
            if (kDup == 1) std::copy(taps_[p].begin(), taps_[p].end(), tapsD_.begin() + (long)ph * T_);
            else for (int t = 0; t < T_; t++) tapsD_[(size_t)ph * T_ * kDup + 2 * t] = tapsD_[(size_t)ph * T_ * kDup + 2 * t + 1] = taps_[p][t];
        }
    }
    reset();
    return true;
}

void RationalResampler::reset() {
    re_.assign(H_ - 1, 0.f);
    im_.assign(H_ - 1, 0.f);
    buf_.assign(H_ - 1, cf32(0, 0));
    head_ = 0;
}

void RationalResampler::process(const cf32* in, size_t n, std::vector<cf32>& out) {
    if (passthrough()) { out.insert(out.end(), in, in + n); return; }
    if (inter_) processInterleaved(in, n, out);
    else processSplit(in, n, out);
}

#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
namespace {
// N outputs at once (their sums are independent chains: one alone waits on the latency of each multiply-add). x: the first input sample
// of each (interleaved I Q), h: its T taps. vld2 splits 4 samples into I and Q, so the taps are used as they are. The partial sums and the
// final additions are those of the portable desamp() (dsp_compat.cpp): 8 partial sums per output (tap k into sum k mod 8, fused
// multiply-adds), added as ((s0 + s4) + (s1 + s5)) + ((s2 + s6) + (s3 + s7)).
template <int N>
inline void dotN(const float* const* x, const float* const* h, int T, cf32* o) {
    float32x4_t r0[N], r1[N], i0[N], i1[N];
    for (int j = 0; j < N; j++) r0[j] = r1[j] = i0[j] = i1[j] = vdupq_n_f32(0.f);
    for (int t = 0; t < T; t += 8)
        for (int j = 0; j < N; j++) {
            const float32x4x2_t a = vld2q_f32(x[j] + 2 * t), b = vld2q_f32(x[j] + 2 * t + 8);
            const float32x4_t h0 = vld1q_f32(h[j] + t), h1 = vld1q_f32(h[j] + t + 4);
            r0[j] = vfmaq_f32(r0[j], h0, a.val[0]); i0[j] = vfmaq_f32(i0[j], h0, a.val[1]);
            r1[j] = vfmaq_f32(r1[j], h1, b.val[0]); i1[j] = vfmaq_f32(i1[j], h1, b.val[1]);
        }
    for (int j = 0; j < N; j++) {
        // (s0+s4, s1+s5, s2+s6, s3+s7) for I and Q, then pairwise twice: (s0+s4)+(s1+s5), (s2+s6)+(s3+s7), and their sum
        const float32x4_t p = vpaddq_f32(vaddq_f32(r0[j], r1[j]), vaddq_f32(i0[j], i1[j]));   // I01 I23 Q01 Q23
        const float32x4_t q = vpaddq_f32(p, p);                                               // I Q I Q
        vst1_f32(reinterpret_cast<float*>(o + j), vget_low_f32(q));
    }
}
} // namespace
#endif

#if defined(DECT2_X86_DISPATCH)
namespace {
// four outputs at once: x[q] the first input sample (interleaved I Q) and h[q] the T taps (each twice) of output q
DECT2_AVX2_TARGET void dot4Avx2(const float* const* x, const float* const* h, int T, cf32* o) {
    __m256 a0 = _mm256_setzero_ps(), a1 = a0, a2 = a0, a3 = a0;
    for (int t = 0; t < T; t += 4) {
        a0 = _mm256_fmadd_ps(_mm256_loadu_ps(x[0] + 2 * t), _mm256_loadu_ps(h[0] + 2 * t), a0);
        a1 = _mm256_fmadd_ps(_mm256_loadu_ps(x[1] + 2 * t), _mm256_loadu_ps(h[1] + 2 * t), a1);
        a2 = _mm256_fmadd_ps(_mm256_loadu_ps(x[2] + 2 * t), _mm256_loadu_ps(h[2] + 2 * t), a2);
        a3 = _mm256_fmadd_ps(_mm256_loadu_ps(x[3] + 2 * t), _mm256_loadu_ps(h[3] + 2 * t), a3);
    }
    __m256 a[4] = {a0, a1, a2, a3};
    for (int q = 0; q < 4; q++) {
        __m128 s = _mm_add_ps(_mm256_castps256_ps128(a[q]), _mm256_extractf128_ps(a[q], 1));
        s = _mm_add_ps(s, _mm_movehl_ps(s, s));
        _mm_storel_pi(reinterpret_cast<__m64*>(o + q), s);
    }
}
DECT2_AVX2_TARGET void dot1Avx2(const float* x, const float* h, int T, cf32* o) {
    __m256 a = _mm256_setzero_ps(), b = a;
    for (int t = 0; t < T; t += 8) {
        a = _mm256_fmadd_ps(_mm256_loadu_ps(x + 2 * t), _mm256_loadu_ps(h + 2 * t), a);
        b = _mm256_fmadd_ps(_mm256_loadu_ps(x + 2 * t + 8), _mm256_loadu_ps(h + 2 * t + 8), b);
    }
    a = _mm256_add_ps(a, b);
    __m128 s = _mm_add_ps(_mm256_castps256_ps128(a), _mm256_extractf128_ps(a, 1));
    s = _mm_add_ps(s, _mm_movehl_ps(s, s));
    _mm_storel_pi(reinterpret_cast<__m64*>(o), s);
}
} // namespace
#endif

void RationalResampler::processInterleaved(const cf32* in, size_t n, std::vector<cf32>& out) {
#if (defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)) || defined(DECT2_X86_DISPATCH)
    if (head_ > 0 && head_ >= buf_.size() - head_) {
        buf_.erase(buf_.begin(), buf_.begin() + (long)head_);
        head_ = 0;
    }
    buf_.insert(buf_.end(), in, in + n);
    const long long avail = (long long)(buf_.size() - head_);
    const long long P = (avail - (T_ + M_ + 2)) / M_;
    if (P <= 0) return;
    const size_t o0 = out.size(), total = (size_t)P * L_;
    out.resize(o0 + total);
    cf32* o = out.data() + o0;
    const float* x0 = reinterpret_cast<const float*>(buf_.data() + head_);
    const float* hd = tapsD_.data();
#if defined(DECT2_X86_DISPATCH)
    constexpr int kTapDup = 2;
#else
    constexpr int kTapDup = 1;
#endif
    size_t j = 0;
    int ph = 0; long long base = 0;   // output j = k L + ph starts at input k M + startOf_[ph]
    const float* xs[4]; const float* hs[4];
    auto next = [&](int q) {
        xs[q] = x0 + 2 * (base + startOf_[ph]);
        hs[q] = hd + (size_t)ph * T_ * kTapDup;
        if (++ph == L_) { ph = 0; base += M_; }
    };
    for (; j + 4 <= total; j += 4) {
        next(0); next(1); next(2); next(3);
#if defined(DECT2_X86_DISPATCH)
        dot4Avx2(xs, hs, T_, o + j);
#else
        dotN<4>(xs, hs, T_, o + j);
#endif
    }
#if defined(DECT2_X86_DISPATCH)
    for (; j < total; j++) { next(0); dot1Avx2(xs[0], hs[0], T_, o + j); }
#else
    for (; j < total; j++) { next(0); dotN<1>(xs, hs, T_, o + j); }
#endif
    head_ += (size_t)P * M_;
#else
    processSplit(in, n, out);
#endif
}

void RationalResampler::processSplit(const cf32* in, size_t n, std::vector<cf32>& out) {
    // the consumed part in front is dropped only once it is larger than what is still pending (one move per few blocks, not one per block)
    if (head_ > 0 && head_ >= re_.size() - head_) {
        re_.erase(re_.begin(), re_.begin() + (long)head_);
        im_.erase(im_.begin(), im_.begin() + (long)head_);
        head_ = 0;
    }
    size_t old = re_.size();
    re_.resize(old + n); im_.resize(old + n);
    {
        float* __restrict r = re_.data() + old; float* __restrict m = im_.data() + old;
        const float* f = reinterpret_cast<const float*>(in);
        for (size_t i = 0; i < n; i++) { r[i] = f[2 * i]; m[i] = f[2 * i + 1]; }
    }
    // buffer index head_ corresponds to input sample (-(H-1)); output n uses samples i0 + k, k = -(H-1)..H
    const long long avail = (long long)(re_.size() - head_);
    // periods: each consumes M input samples and yields L outputs; the last needed index is base+kM + T - 1
    long long P = (avail - (T_ + M_ + 2)) / M_;
    if (P <= 0) return;
    // every phase's outputs first (row ph = output phase ph, the same desamp calls as before), then one interleaving pass that writes
    // the output in order: writing each phase straight into the output touched every cache line of it once per phase (32 passes over
    // 480 KB for 10 -> 9.14 Msps)
    const size_t PP = (size_t)P;
    if (tr_.size() < PP * L_) { tr_.resize(PP * L_); ti_.resize(PP * L_); }
    for (int p = 0; p < L_; p++) {
        const float* hp = taps_[p].data();
        const size_t row = (size_t)nOfPhase_[p] * PP;
        desamp(re_.data() + head_ + base_[p], M_, hp, tr_.data() + row, (int)P, T_);
        desamp(im_.data() + head_ + base_[p], M_, hp, ti_.data() + row, (int)P, T_);
    }
    const size_t o0 = out.size();
    out.resize(o0 + PP * L_);
    cf32* o = out.data() + o0;
    const float* tr = tr_.data();
    const float* ti = ti_.data();
    for (size_t k = 0; k < PP; k++)
        for (int ph = 0; ph < L_; ph++) o[k * L_ + ph] = cf32(tr[(size_t)ph * PP + k], ti[(size_t)ph * PP + k]);
    // consumed input, keeping H-1 history
    head_ += (size_t)P * M_;
}

} // namespace dect2
