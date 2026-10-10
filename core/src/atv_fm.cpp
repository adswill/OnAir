#include "atv_fm.h"
#include "atv_dsp.h"
#include <algorithm>
#include <cmath>
#if defined(__GNUC__) && defined(__x86_64__) && !defined(DECT2_NO_SIMD)
#include <immintrin.h>
#define DECT2_FM_AVX2 1
#endif

namespace dect2 {

namespace {

// atan2 to about 1e-5 rad (std::atan2 in double at 20 Msps would be a large part of a core)
inline float fastAtan2(float y, float x) {
    const float ax = std::fabs(x), ay = std::fabs(y);
    const float mx = std::max(ax, ay), mn = std::min(ax, ay);
    const float a = mn / (mx + 1e-30f);
    const float s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0) r = 3.14159274f - r;
    return y < 0 ? -r : r;
}

// the low-pass at every decim-th position from `s` while a whole window fits; returns the first window start not done
size_t filterPortable(const float* b, size_t size, const float* h, int nT, size_t s, int decim, std::vector<float>& out) {
    for (; s + (size_t)nT <= size; s += (size_t)decim) {
        float a[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        const float* w = b + s;
        for (int t = 0; t < nT; t += 8) for (int j = 0; j < 8; j++) a[j] += w[t + j] * h[t + j];
        out.push_back(((a[0] + a[1]) + (a[2] + a[3])) + ((a[4] + a[5]) + (a[6] + a[7])));
    }
    return s;
}

#ifdef DECT2_FM_AVX2
__attribute__((target("avx2")))
size_t filterAvx2(const float* b, size_t size, const float* h, int nT, size_t s, int decim, std::vector<float>& out) {
    for (; s + (size_t)nT <= size; s += (size_t)decim) {
        __m256 a = _mm256_setzero_ps();
        const float* w = b + s;
        for (int t = 0; t < nT; t += 8) a = _mm256_add_ps(a, _mm256_mul_ps(_mm256_loadu_ps(w + t), _mm256_loadu_ps(h + t)));
        __m128 l = _mm_add_ps(_mm256_castps256_ps128(a), _mm256_extractf128_ps(a, 1));
        l = _mm_hadd_ps(l, l);
        l = _mm_hadd_ps(l, l);
        out.push_back(_mm_cvtss_f32(l));
    }
    return s;
}
#endif

size_t filterBlock(const float* b, size_t size, const float* h, int nT, size_t s, int decim, std::vector<float>& out) {
#ifdef DECT2_FM_AVX2
    static const bool avx2 = __builtin_cpu_supports("avx2");
    if (avx2) return filterAvx2(b, size, h, nT, s, decim, out);
#endif
    return filterPortable(b, size, h, nT, s, decim, out);
}

}   // namespace

void AtvFmDemod::configure(double fs) {
    fs_ = fs;
    decim_ = std::max(1, (int)std::lround(fs / 10e6));
    fv_ = fs / decim_;
    // the video band (a colour subcarrier at 4.43 MHz with its sidebands) passes, what is above 5.4 MHz (the audio subcarriers at 6 MHz and more,
    // and what would fold into the band at 10 Msps) is down by 45 dB
    taps_ = atvdsp::lowpass(std::min(4.6e6, 0.46 * fv_), std::min(5.4e6, 0.54 * fv_), fs, 45, 255);
    nTaps_ = (int)((taps_.size() + 7) & ~(size_t)7);
    taps_.resize((size_t)nTaps_, 0.f);
    reset();
}

namespace {
constexpr int kHilbertHalf = 15;   // the Hilbert filter has 31 taps and a delay of 15 samples (1.5 us at 10 Msps)
}

void AtvFmDemod::reset() {
    prev_ = cf32(1, 0);
    phase_ = 0;
    buf_.assign((size_t)nTaps_, 0.f);
    out_.clear();
    hist_.assign((size_t)2 * kHilbertHalf, 0.f);
    v_.clear(); i_.clear(); q_.clear();
    if (hil_.empty()) {   // h[k] = 2 / (pi k) for odd k, Hamming window; h is odd in k
        for (int j = 0; j < (kHilbertHalf + 1) / 2; j++) {
            const int k = 2 * j + 1;
            hil_.push_back((float)(2.0 / (M_PI * k) * (0.54 + 0.46 * std::cos(M_PI * k / (kHilbertHalf + 1)))));
        }
    }
}

size_t AtvFmDemod::process(const cf32* x, size_t n) {
    out_.clear();
    if (nTaps_ == 0 || !n) return 0;
    // radians per sample -> Hz -> a swing of 16 MHz peak to peak is about 1; sync tip at the maximum
    const float sc = (float)((syncLow_ ? -1.0 : 1.0) * fs_ / (2 * M_PI) / 8e6);
    const size_t hist = buf_.size();
    buf_.resize(hist + n);
    float* d = buf_.data() + hist;
    cf32 p = prev_;
    for (size_t k = 0; k < n; k++) {
        const cf32 z = x[k] * std::conj(p);
        d[k] = fastAtan2(z.imag(), z.real()) * sc;
        p = x[k];
    }
    prev_ = p;
    const size_t s = filterBlock(buf_.data(), buf_.size(), taps_.data(), nTaps_, phase_, decim_, out_);
    // forget what the next window no longer reaches back to
    const size_t drop = std::min(s, buf_.size());
    buf_.erase(buf_.begin(), buf_.begin() + (std::ptrdiff_t)drop);
    phase_ = s - drop;
    // the analytic signal: output m (delayed by kHilbertHalf) reads hist_[m - 15 .. m + 15]
    const size_t nOut = out_.size();
    hist_.insert(hist_.end(), out_.begin(), out_.end());
    v_.resize(nOut); i_.resize(nOut); q_.resize(nOut);
    const int D = kHilbertHalf;
    for (size_t m = 0; m < nOut; m++) {
        const float* c = &hist_[m + (size_t)D];    // the sample at the delay
        float acc = 0;
        for (int j = 0; j < (int)hil_.size(); j++) {
            const int k = 2 * j + 1;
            acc += hil_[(size_t)j] * (c[-k] - c[k]);   // sum over lags +-k of h[k] x[m - k]: h odd
        }
        v_[m] = *c; i_[m] = *c; q_[m] = acc;
    }
    hist_.erase(hist_.begin(), hist_.begin() + (std::ptrdiff_t)nOut);
    return nOut;
}

} // namespace dect2
