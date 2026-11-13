// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/dsp/exact_resampler.h"
#if defined(__GNUC__) && defined(__x86_64__) && !defined(DECT2_NO_SIMD)
#include <immintrin.h>
#define DECT2_ER_AVX2 1
#endif
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
#include <arm_neon.h>
#define DECT2_ER_NEON 1
#endif
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include "dect2/common/window.h"

namespace dect2 {

namespace {
double besselI0(double x) { return dect2::window::besselI0(x, 40, 0.0); }

// Kaiser window value of the tap at distance x (|x| < half): the same expression as ever
inline double winAt(double x, int half, double beta, double i0b) {
    return std::fabs(x) < half ? besselI0(beta * std::sqrt(1.0 - (x / half) * (x / half))) / i0b : 0.0;
}

#ifdef DECT2_ER_AVX2
// Four taps at a time: the same IEEE operations in the same order as winAt / window::besselI0 (division, multiplication, addition and the
// square root are correctly rounded in a vector as in a scalar; no FMA is available in this target, so nothing is contracted): the same bits.
__attribute__((target("avx2")))
void windowRowAvx2(const double* x, int n, int half, double beta, double i0b, double* w) {
    const __m256d vh = _mm256_set1_pd((double)half), vb = _mm256_set1_pd(beta), vi = _mm256_set1_pd(i0b), one = _mm256_set1_pd(1.0);
    const __m256d sign = _mm256_set1_pd(-0.0);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        const __m256d xv = _mm256_loadu_pd(x + i);
        const __m256d r = _mm256_div_pd(xv, vh);
        const __m256d z = _mm256_mul_pd(vb, _mm256_sqrt_pd(_mm256_sub_pd(one, _mm256_mul_pd(r, r))));
        __m256d s = one, t = one;
        for (int k = 1; k < 40; k++) {
            const __m256d q = _mm256_div_pd(z, _mm256_set1_pd(2.0 * k));
            t = _mm256_mul_pd(t, _mm256_mul_pd(q, q));
            s = _mm256_add_pd(s, t);
        }
        const __m256d val = _mm256_div_pd(s, vi);
        const __m256d inside = _mm256_cmp_pd(_mm256_andnot_pd(sign, xv), vh, _CMP_LT_OQ);
        _mm256_storeu_pd(w + i, _mm256_and_pd(val, inside));
    }
    for (; i < n; i++) w[i] = winAt(x[i], half, beta, i0b);
}
#endif

void windowRow(const double* x, int n, int half, double beta, double i0b, double* w) {
#ifdef DECT2_ER_AVX2
    static const bool avx2 = __builtin_cpu_supports("avx2");
    if (avx2) { windowRowAvx2(x, n, half, beta, i0b, w); return; }
#endif
    for (int i = 0; i < n; i++) w[i] = winAt(x[i], half, beta, i0b);
}

// The filter bank: (phases + 1) rows of `stride` taps. Row p' = phases - p is row p read backwards (x -> -x exactly: the positions are dyadic,
// and the window and the sinc are even and computed from |x|-symmetric operations), so only half the rows are evaluated; each row is still
// normalised by its own sum taken in tap order, as before.
std::vector<float> buildBank(double down, int half, int taps, int stride, int phases) {
    const double fc = 0.5 * down * 0.97;
    std::vector<float> bank((size_t)(phases + 1) * (size_t)stride, 0.f);
    const double beta = 8.0, i0b = besselI0(beta);
    std::vector<double> x((size_t)taps), w((size_t)taps), t((size_t)taps), u((size_t)taps);
    for (int p = 0; p <= phases / 2; p++) {
        const double tau = (double)p / phases;
        for (int k = 0; k < taps; k++) x[(size_t)k] = (k - (half - 1)) - tau;
        windowRow(x.data(), taps, half, beta, i0b, w.data());
        double sum = 0;
        for (int k = 0; k < taps; k++) {
            const double xk = x[(size_t)k];
            const double sc = xk == 0 ? 1.0 : std::sin(2 * M_PI * fc * xk) / (2 * M_PI * fc * xk);
            t[(size_t)k] = 2 * fc * sc * w[(size_t)k];
            sum += t[(size_t)k];
        }
        float* row = &bank[(size_t)p * (size_t)stride];
        for (int k = 0; k < taps; k++) row[k] = (float)(t[(size_t)k] / sum);
        const int q = phases - p;
        if (q != p) {
            double sum2 = 0;
            for (int k = 0; k < taps; k++) { u[(size_t)k] = t[(size_t)(taps - 1 - k)]; sum2 += u[(size_t)k]; }
            float* row2 = &bank[(size_t)q * (size_t)stride];
            for (int k = 0; k < taps; k++) row2[k] = (float)(u[(size_t)k] / sum2);
        }
    }
    return bank;
}
}

bool ExactResampler::configure(double inRate, double outRate, int lobes) {
    if (inRate <= 0 || outRate <= 0 || outRate > 4 * inRate) return false;
    const double ratio = outRate / inRate;
    step_ = inRate / outRate;
    pass_ = std::fabs(ratio - 1.0) < 1e-12;
    if (pass_) { reset(); return true; }
    down_ = std::min(1.0, ratio);
    half_ = (int)std::ceil((double)lobes / down_);
    taps_ = 2 * half_;
    stride_ = (taps_ + 7) & ~7;   // a multiple of the vector width of the dot product below
    // The bank is built when the first samples arrive (every receiver of the engine is configured at start, most are never fed) and shared
    // between resamplers of the same ratio and length.
    bank_.reset();
    reset();
    return true;
}

void ExactResampler::ensureBank() {
    if (bank_) return;
    struct Key { double down; int half; bool operator<(const Key& o) const { return down != o.down ? down < o.down : half < o.half; } };
    static std::mutex mu;
    static std::map<Key, std::weak_ptr<const std::vector<float>>> cache;
    const Key key{down_, half_};
    {
        std::lock_guard<std::mutex> lk(mu);
        auto it = cache.find(key);
        if (it != cache.end()) { bank_ = it->second.lock(); if (bank_) return; }
    }
    auto b = std::make_shared<const std::vector<float>>(buildBank(down_, half_, taps_, stride_, phases_));
    std::lock_guard<std::mutex> lk(mu);
    auto& slot = cache[key];
    if (auto o = slot.lock()) bank_ = o; else { slot = b; bank_ = b; }
}

void ExactResampler::reset() {
    re_.assign((size_t)(half_ + 2), 0.f);   // history of silence in front of the first sample
    im_.assign(re_.size(), 0.f);
    pos_ = (double)re_.size();               // the first output sits on the first input sample
}

// The resampling loop: one windowed-sinc dot product (real and imaginary part, stride taps) per output sample. Eight independent
// partial sums let the compiler use 8-wide vectors (AVX2 where the CPU has it, picked at load).
namespace {
size_t resampleLoopPortable(const float* R, const float* M, const float* B, int stride, int phases, int hm1, long avail, double& posRef, double step, cf32* o) {
    size_t produced = 0;
    double pos = posRef;
    for (;;) {
        const long i0 = (long)pos;   // pos is never negative
        const long base = i0 - hm1;
        if (base + stride > avail) break;   // the dot product runs over the padded length
        const int p = (int)((pos - (double)i0) * phases + 0.5);
        const float* h = B + (size_t)p * (size_t)stride;
        const float* r = R + base;
        const float* m = M + base;
        float ar[8] = {0, 0, 0, 0, 0, 0, 0, 0}, ai[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        for (int t = 0; t < stride; t += 8)
            for (int j = 0; j < 8; j++) { ar[j] += r[t + j] * h[t + j]; ai[j] += m[t + j] * h[t + j]; }
        o[produced++] = cf32(((ar[0] + ar[1]) + (ar[2] + ar[3])) + ((ar[4] + ar[5]) + (ar[6] + ar[7])),
                             ((ai[0] + ai[1]) + (ai[2] + ai[3])) + ((ai[4] + ai[5]) + (ai[6] + ai[7])));
        pos += step;
    }
    posRef = pos;
    return produced;
}

#ifdef DECT2_ER_NEON
// The same loop with NEON: two 4-lane accumulators are the eight partial sums of the portable loop (lane j of A0 / A1 is its ar[j] / ar[4 + j]),
// fused multiply-adds like the portable loop gets from the compiler, and the lanes are added in the same tree, so the result is bit-identical.
// (The compiler's own vectorisation of the portable loop was two lanes wide with a zip per vector: three times the instructions.)
// Two outputs per pass: their chains are independent, so one's four-cycle multiply-add latency is hidden behind the other's.
inline cf32 neonJoin(float32x4_t r0, float32x4_t r1, float32x4_t m0, float32x4_t m1) {
    const float32x4_t pr = vpaddq_f32(r0, r1), pm = vpaddq_f32(m0, m1);   // (0+1) (2+3) (4+5) (6+7)
    const float32x4_t qr = vpaddq_f32(pr, pm);                            // re: (01+23) (45+67), im: the same
    return cf32(vgetq_lane_f32(qr, 0) + vgetq_lane_f32(qr, 1), vgetq_lane_f32(qr, 2) + vgetq_lane_f32(qr, 3));
}
size_t resampleLoopNeon(const float* R, const float* M, const float* B, int stride, int phases, int hm1, long avail, double& posRef, double step, cf32* o) {
    size_t produced = 0;
    double pos = posRef;
    auto where = [&](double ps, long& base, const float*& h) {
        const long i0 = (long)ps;
        base = i0 - hm1;
        h = B + (size_t)((int)((ps - (double)i0) * phases + 0.5)) * (size_t)stride;
    };
    for (;;) {
        long baseA; const float* hA;
        where(pos, baseA, hA);
        if (baseA + stride > avail) break;
        long baseB; const float* hB;
        const double posB = pos + step;
        where(posB, baseB, hB);
        const float *rA = R + baseA, *mA = M + baseA;
        float32x4_t r0 = vdupq_n_f32(0), r1 = r0, m0 = r0, m1 = r0;
        if (baseB + stride <= avail) {
            const float *rB = R + baseB, *mB = M + baseB;
            float32x4_t s0 = r0, s1 = r0, n0 = r0, n1 = r0;
            for (int t = 0; t < stride; t += 8) {
                const float32x4_t h0 = vld1q_f32(hA + t), h1 = vld1q_f32(hA + t + 4), g0 = vld1q_f32(hB + t), g1 = vld1q_f32(hB + t + 4);
                r0 = vfmaq_f32(r0, vld1q_f32(rA + t), h0); r1 = vfmaq_f32(r1, vld1q_f32(rA + t + 4), h1);
                s0 = vfmaq_f32(s0, vld1q_f32(rB + t), g0); s1 = vfmaq_f32(s1, vld1q_f32(rB + t + 4), g1);
                m0 = vfmaq_f32(m0, vld1q_f32(mA + t), h0); m1 = vfmaq_f32(m1, vld1q_f32(mA + t + 4), h1);
                n0 = vfmaq_f32(n0, vld1q_f32(mB + t), g0); n1 = vfmaq_f32(n1, vld1q_f32(mB + t + 4), g1);
            }
            o[produced++] = neonJoin(r0, r1, m0, m1);
            o[produced++] = neonJoin(s0, s1, n0, n1);
            pos = posB + step;
        } else {
            for (int t = 0; t < stride; t += 8) {
                const float32x4_t h0 = vld1q_f32(hA + t), h1 = vld1q_f32(hA + t + 4);
                r0 = vfmaq_f32(r0, vld1q_f32(rA + t), h0); r1 = vfmaq_f32(r1, vld1q_f32(rA + t + 4), h1);
                m0 = vfmaq_f32(m0, vld1q_f32(mA + t), h0); m1 = vfmaq_f32(m1, vld1q_f32(mA + t + 4), h1);
            }
            o[produced++] = neonJoin(r0, r1, m0, m1);
            pos = posB;
        }
    }
    posRef = pos;
    return produced;
}
#endif

#ifdef DECT2_ER_AVX2
// The same loop with 256-bit vectors: stride is a multiple of 8, the real and imaginary rows share the tap row loaded once.
__attribute__((target("avx2,fma")))
static inline __m128 hsum4(__m256 a, __m256 b, __m256 c, __m256 d) {
    const __m128 x = _mm_add_ps(_mm256_castps256_ps128(a), _mm256_extractf128_ps(a, 1)), y = _mm_add_ps(_mm256_castps256_ps128(b), _mm256_extractf128_ps(b, 1));
    const __m128 z = _mm_add_ps(_mm256_castps256_ps128(c), _mm256_extractf128_ps(c, 1)), w = _mm_add_ps(_mm256_castps256_ps128(d), _mm256_extractf128_ps(d, 1));
    return _mm_hadd_ps(_mm_hadd_ps(x, y), _mm_hadd_ps(z, w));
}
__attribute__((target("avx2,fma")))
size_t resampleLoopAvx2(const float* R, const float* M, const float* B, int stride, int phases, int hm1, long avail, double& posRef, double step, cf32* o) {
    size_t produced = 0;
    double pos = posRef;
    for (;;) {   // four outputs per pass: their eight accumulator chains are independent, so the multiply-add latency is hidden
        long base[4]; const float* h[4];
        double q = pos;
        bool ok = true;
        for (int u = 0; u < 4; u++) {
            const long i0 = (long)q;
            base[u] = i0 - hm1;
            if (base[u] + stride > avail) { ok = false; break; }
            h[u] = B + (size_t)((int)((q - (double)i0) * phases + 0.5)) * (size_t)stride;
            q += step;
        }
        if (!ok) break;
        __m256 ar[4], ai[4];
        for (int u = 0; u < 4; u++) ar[u] = ai[u] = _mm256_setzero_ps();
        for (int t = 0; t < stride; t += 8) {
            for (int u = 0; u < 4; u++) {
                const __m256 hv = _mm256_loadu_ps(h[u] + t);
                ar[u] = _mm256_fmadd_ps(_mm256_loadu_ps(R + base[u] + t), hv, ar[u]);
                ai[u] = _mm256_fmadd_ps(_mm256_loadu_ps(M + base[u] + t), hv, ai[u]);
            }
        }
        float re4[4], im4[4];
        _mm_storeu_ps(re4, hsum4(ar[0], ar[1], ar[2], ar[3]));
        _mm_storeu_ps(im4, hsum4(ai[0], ai[1], ai[2], ai[3]));
        for (int u = 0; u < 4; u++) o[produced++] = cf32(re4[u], im4[u]);
        pos = q;
    }
    for (;;) {
        const long i0 = (long)pos;
        const long base = i0 - hm1;
        if (base + stride > avail) break;
        const int p = (int)((pos - (double)i0) * phases + 0.5);
        const float* h = B + (size_t)p * (size_t)stride;
        const float* r = R + base;
        const float* m = M + base;
        __m256 ar = _mm256_setzero_ps(), ai = _mm256_setzero_ps();
        for (int t = 0; t < stride; t += 8) {
            const __m256 hv = _mm256_loadu_ps(h + t);
            ar = _mm256_add_ps(ar, _mm256_mul_ps(_mm256_loadu_ps(r + t), hv));
            ai = _mm256_add_ps(ai, _mm256_mul_ps(_mm256_loadu_ps(m + t), hv));
        }
        // add the two halves, then the pairs, then the two lanes left (the sums of the portable loop in another order)
        __m128 lr = _mm_add_ps(_mm256_castps256_ps128(ar), _mm256_extractf128_ps(ar, 1));
        __m128 li = _mm_add_ps(_mm256_castps256_ps128(ai), _mm256_extractf128_ps(ai, 1));
        lr = _mm_hadd_ps(lr, li);   // r0+r1, r2+r3, i0+i1, i2+i3
        lr = _mm_hadd_ps(lr, lr);   // re, im, re, im
        o[produced++] = cf32(_mm_cvtss_f32(lr), _mm_cvtss_f32(_mm_shuffle_ps(lr, lr, 1)));
        pos += step;
    }
    posRef = pos;
    return produced;
}
#endif

}

size_t resampleLoop(const float* R, const float* M, const float* B, int stride, int phases, int hm1, long avail, double& pos, double step, cf32* o) {
#ifdef DECT2_ER_AVX2
    static const bool avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
    if (avx2) return resampleLoopAvx2(R, M, B, stride, phases, hm1, avail, pos, step, o);
#endif
#ifdef DECT2_ER_NEON
    return resampleLoopNeon(R, M, B, stride, phases, hm1, avail, pos, step, o);
#endif
    return resampleLoopPortable(R, M, B, stride, phases, hm1, avail, pos, step, o);
}

void ExactResampler::process(const cf32* in, size_t n, std::vector<cf32>& out) {
    if (!n) return;
    if (pass_) { out.insert(out.end(), in, in + n); return; }
    {   // append the input as separate real and imaginary rows (one resize, not a push_back per sample)
        const size_t at = re_.size();
        re_.resize(at + n); im_.resize(at + n);
        float* pr = &re_[at]; float* pi = &im_[at];
        for (size_t i = 0; i < n; i++) { pr[i] = in[i].real(); pi[i] = in[i].imag(); }
    }
    ensureBank();
    const long avail = (long)re_.size();
    const double endPos = (double)(avail - stride_ + half_);   // outputs are made while pos < endPos: the dot product still fits in the input
    const size_t first = out.size();
    out.resize(first + (size_t)std::max(0.0, std::ceil((endPos - pos_) / step_)) + 2);
    cf32* o = out.data() + first;
    const size_t produced = resampleLoop(re_.data(), im_.data(), bank_->data(), stride_, phases_, half_ - 1, avail, pos_, step_, o);
    out.resize(first + produced);
    // drop what is no longer needed, keeping the history the next output needs
    const long keepFrom = (long)std::floor(pos_) - (half_ - 1);
    if (keepFrom > 4096) {
        re_.erase(re_.begin(), re_.begin() + keepFrom);
        im_.erase(im_.begin(), im_.begin() + keepFrom);
        pos_ -= (double)keepFrom;
    }
}

} // namespace dect2
