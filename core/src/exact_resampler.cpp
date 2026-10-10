#include "dect2/exact_resampler.h"
#if defined(__GNUC__) && defined(__x86_64__) && !defined(DECT2_NO_SIMD)
#include <immintrin.h>
#define DECT2_ER_AVX2 1
#endif
#include <algorithm>
#include <cmath>

namespace dect2 {

namespace {
double besselI0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 40; k++) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; }
    return s;
}
}

bool ExactResampler::configure(double inRate, double outRate) {
    if (inRate <= 0 || outRate <= 0 || outRate > 4 * inRate) return false;
    const double ratio = outRate / inRate;
    step_ = inRate / outRate;
    pass_ = std::fabs(ratio - 1.0) < 1e-12;
    if (pass_) { reset(); return true; }
    const double down = std::min(1.0, ratio);
    const double fc = 0.5 * down * 0.97;
    half_ = (int)std::ceil(16.0 / down);
    taps_ = 2 * half_;
    stride_ = (taps_ + 7) & ~7;   // a multiple of the vector width of the dot product below
    bank_.assign((size_t)(phases_ + 1) * (size_t)stride_, 0.f);
    const double beta = 8.0, i0b = besselI0(beta);
    std::vector<double> t((size_t)taps_);
    for (int p = 0; p <= phases_; p++) {
        const double tau = (double)p / phases_;
        double sum = 0;
        for (int k = 0; k < taps_; k++) {
            const double x = (k - (half_ - 1)) - tau;
            const double w = std::fabs(x) < half_ ? besselI0(beta * std::sqrt(1.0 - (x / half_) * (x / half_))) / i0b : 0.0;
            const double sc = x == 0 ? 1.0 : std::sin(2 * M_PI * fc * x) / (2 * M_PI * fc * x);
            t[(size_t)k] = 2 * fc * sc * w;
            sum += t[(size_t)k];
        }
        for (int k = 0; k < taps_; k++) bank_[(size_t)p * (size_t)stride_ + (size_t)k] = (float)(t[(size_t)k] / sum);
    }
    reset();
    return true;
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

#ifdef DECT2_ER_AVX2
// The same loop with 256-bit vectors: stride is a multiple of 8, the real and imaginary rows share the tap row loaded once.
__attribute__((target("avx2")))
size_t resampleLoopAvx2(const float* R, const float* M, const float* B, int stride, int phases, int hm1, long avail, double& posRef, double step, cf32* o) {
    size_t produced = 0;
    double pos = posRef;
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

size_t resampleLoop(const float* R, const float* M, const float* B, int stride, int phases, int hm1, long avail, double& pos, double step, cf32* o) {
#ifdef DECT2_ER_AVX2
    static const bool avx2 = __builtin_cpu_supports("avx2");
    if (avx2) return resampleLoopAvx2(R, M, B, stride, phases, hm1, avail, pos, step, o);
#endif
    return resampleLoopPortable(R, M, B, stride, phases, hm1, avail, pos, step, o);
}
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
    const long avail = (long)re_.size();
    const double endPos = (double)(avail - stride_ + half_);   // outputs are made while pos < endPos: the dot product still fits in the input
    const size_t first = out.size();
    out.resize(first + (size_t)std::max(0.0, std::ceil((endPos - pos_) / step_)) + 2);
    cf32* o = out.data() + first;
    const size_t produced = resampleLoop(re_.data(), im_.data(), bank_.data(), stride_, phases_, half_ - 1, avail, pos_, step_, o);
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
