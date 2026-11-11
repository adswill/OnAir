#include "dect2/dsp/dsp_compat.h"
#include "dect2/dsp/simd.h"
#if defined(DECT2_X86_DISPATCH)
#include <immintrin.h>
#endif
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#if defined(__APPLE__) && !defined(DECT2_PORTABLE)
#define DECT2_USE_ACCELERATE 1
#include <Accelerate/Accelerate.h>
#include <algorithm>
#endif

namespace dect2 {

// Short filters (the channel-estimate interpolators have 16 taps): eight outputs at a time, one broadcast tap against eight
// neighbouring inputs per step, instead of one dot product (with its reduction and call overhead) per output.
// Thirty-two outputs at a time first: each output is still one running sum over the taps in order (the same value bit for bit), but eight vector
// sums run side by side instead of two, so the chain of dependent multiply-adds (16 deep) no longer sets the speed.
DECT2_SIMD_BODY void convCorrShortBody(const float* a, const float* f, float* c, int strideC, int n, int p) {
    int i = 0;
    for (; i + 32 <= n; i += 32) {
        float acc[32];
        for (int l = 0; l < 32; l++) acc[l] = 0;
        const float* x = a + i;
        for (int k = 0; k < p; k++) { const float fk = f[k]; for (int l = 0; l < 32; l++) acc[l] += fk * x[k + l]; }
        float* o = c + (size_t)i * strideC;   // (a pointer stepped by the stride: no address multiply per store)
        if (strideC == 1) std::memcpy(o, acc, sizeof acc);
        else for (int l = 0; l < 32; l++) { *o = acc[l]; o += strideC; }
    }
    for (; i + 8 <= n; i += 8) {
        float acc[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        const float* x = a + i;
        for (int k = 0; k < p; k++) { const float fk = f[k]; for (int l = 0; l < 8; l++) acc[l] += fk * x[k + l]; }
        for (int l = 0; l < 8; l++) c[(size_t)(i + l) * strideC] = acc[l];
    }
    for (; i < n; i++) {
        float s = 0;
        for (int k = 0; k < p; k++) s += f[k] * a[i + k];
        c[(size_t)i * strideC] = s;
    }
}
DECT2_AVX2_DISPATCH(void, convCorrShort, (const float* a, const float* f, float* c, int strideC, int n, int p), (a, f, c, strideC, n, p))

#ifdef DECT2_USE_ACCELERATE

void fftSplit(float* re, float* im, int log2n, bool inverse) {
    static std::atomic<FFTSetup> setups[24];
    FFTSetup s = setups[log2n].load(std::memory_order_acquire);
    if (!s) {
        FFTSetup fresh = vDSP_create_fftsetup(log2n, kFFTRadix2);
        FFTSetup expected = nullptr;
        if (setups[log2n].compare_exchange_strong(expected, fresh)) s = fresh;
        else { vDSP_destroy_fftsetup(fresh); s = expected; }
    }
    DSPSplitComplex sc{re, im};
    vDSP_fft_zip(s, &sc, 1, log2n, inverse ? FFT_INVERSE : FFT_FORWARD);
}
void convCorr(const float* a, const float* f, float* c, int strideC, int n, int p) {
    if (p <= 32) convCorrShort(a, f, c, strideC, n, p);
    else vDSP_conv(a, 1, f, 1, c, strideC, (vDSP_Length)n, (vDSP_Length)p);
}
void desamp(const float* a, int decim, const float* f, float* c, int n, int p) { vDSP_desamp(a, decim, f, c, (vDSP_Length)n, (vDSP_Length)p); }
void hannWindowNorm(float* w, int n) { vDSP_hann_window(w, n, vDSP_HANN_NORM); }
void syrkLowerT(const float* x, int rows, int n, float* a) { cblas_ssyrk(CblasRowMajor, CblasLower, CblasTrans, n, rows, 1.f, x, n, 0.f, a, n); }
void gemvT(const float* x, int rows, int n, const float* t, float* b) { cblas_sgemv(CblasRowMajor, CblasTrans, rows, n, 1.f, x, n, t, 1, 0.f, b, 1); }

#else  // ------------------------------------------------------------------ portable versions

// NEON stage passes for the FFT (clang and GCC: each with the contraction its own scalar loop gets, so the bits stay those of the loop)
#if defined(__ARM_NEON) && defined(__GNUC__) && !defined(DECT2_NO_SIMD)
#define DECT2_FFT_NEON 1
#include <arm_neon.h>
#else
#define DECT2_FFT_NEON 0
#endif

namespace {
// Radix-2 decimation in time on split arrays. Twiddles are stored per stage (contiguous) so that the butterfly loops vectorise.
struct Plan {
    int n = 0;
    std::vector<uint32_t> rev;
    std::vector<float> wr, wi;   // stage with half-length h uses entries [h - 1, 2h - 1)
    explicit Plan(int log2n) : n(1 << log2n), rev(n), wr(n), wi(n) {
        for (int i = 0; i < n; i++) {
            uint32_t r = 0;
            for (int b = 0; b < log2n; b++) if (i & (1 << b)) r |= 1u << (log2n - 1 - b);
            rev[i] = r;
        }
        for (int h = 1; h < n; h <<= 1)
            for (int k = 0; k < h; k++) {
                const double a = -M_PI * k / h;
                wr[h - 1 + k] = (float)std::cos(a);
                wi[h - 1 + k] = (float)std::sin(a);
            }
    }
};

const Plan& planFor(int log2n) {
    static std::atomic<Plan*> plans[24];
    static std::mutex mu;
    Plan* p = plans[log2n].load(std::memory_order_acquire);
    if (p) return *p;
    std::lock_guard<std::mutex> lk(mu);
    p = plans[log2n].load(std::memory_order_relaxed);
    if (!p) { p = new Plan(log2n); plans[log2n].store(p, std::memory_order_release); }   // plans live for the whole run
    return *p;
}

#if DECT2_FFT_NEON
// The stages from half-length 4 on, two or three per pass over the data instead of one: for a block of 8 h values, the 8 values
// j + m h (m = 0..7) only meet each other in the stages h, 2h and 4h, so they are loaded once (four j at a time), go through the three
// stages in registers and are stored once. Each butterfly is the one of the scalar loop with the same rounding (clang fuses the first product
// of tr = wr br - wi bi and ti = wr bi + wi br, GCC the second; checked with both: the same bits, tests/dsp/test_fft_bits.cpp).
// Fewer loads and stores per butterfly (5 passes instead of 13 for 32768 points).
static inline void bfly(float32x4_t& ar, float32x4_t& ai, float32x4_t& br, float32x4_t& bi, float32x4_t wr, float32x4_t wi) {
#if defined(__clang__)   // clang fuses the first product of each sum, GCC the second (checked against the scalar loop of each)
    const float32x4_t tr = vfmaq_f32(vnegq_f32(vmulq_f32(wi, bi)), wr, br);
    const float32x4_t ti = vfmaq_f32(vmulq_f32(wi, br), wr, bi);
#else
    const float32x4_t tr = vfmsq_f32(vmulq_f32(wr, br), wi, bi);
    const float32x4_t ti = vfmaq_f32(vmulq_f32(wr, bi), wi, br);
#endif
    br = vsubq_f32(ar, tr); bi = vsubq_f32(ai, ti);
    ar = vaddq_f32(ar, tr); ai = vaddq_f32(ai, ti);
}

template <int S>   // S stages (1..3) in one pass, starting at half-length h
static void stagesPass(float* __restrict re, float* __restrict im, int n, int h, const float* wr, const float* wi) {
    constexpr int R = 1 << S;
    for (int i = 0; i < n; i += R * h)
        for (int j = 0; j < h; j += 4) {
            float32x4_t xr[R], xi[R];
            for (int m = 0; m < R; m++) { xr[m] = vld1q_f32(re + i + j + m * h); xi[m] = vld1q_f32(im + i + j + m * h); }
            for (int s = 0; s < S; s++) {
                const int H = h << s, d = 1 << s;   // this stage pairs rows m and m + d (m with bit s clear); half-length H
                for (int m = 0; m < R; m++) {
                    if (m & d) continue;
                    const int k = H - 1 + j + (m & (d - 1)) * h;   // the twiddle of position j + (m mod d) h within the 2H group
                    bfly(xr[m], xi[m], xr[m + d], xi[m + d], vld1q_f32(wr + k), vld1q_f32(wi + k));
                }
            }
            for (int m = 0; m < R; m++) { vst1q_f32(re + i + j + m * h, xr[m]); vst1q_f32(im + i + j + m * h, xi[m]); }
        }
}

static void fftStagesNeon(float* re, float* im, int n, const float* wr, const float* wi) {
    int h = 4;
    while (h < n) {
        if (8 * h <= n) { stagesPass<3>(re, im, n, h, wr, wi); h *= 8; }
        else if (4 * h <= n) { stagesPass<2>(re, im, n, h, wr, wi); h *= 4; }
        else { stagesPass<1>(re, im, n, h, wr, wi); h *= 2; }
    }
}
#else
// The same passes in plain C++ (x86: compiled into the baseline and the AVX2 copy of forward()). The butterfly is written exactly as in
// the one-stage loop, so each compiler contracts it the same way (checked: the same bits, tests/dsp/test_fft_bits.cpp); the j loop vectorises.
template <int S>
DECT2_SIMD_BODY void stagesPassGeneric(float* __restrict re, float* __restrict im, int n, int h, const float* __restrict wr, const float* __restrict wi) {
    constexpr int R = 1 << S;
    for (int i = 0; i < n; i += R * h)
        for (int j = 0; j < h; j++) {
            float xr[R], xi[R];
            for (int m = 0; m < R; m++) { xr[m] = re[i + j + m * h]; xi[m] = im[i + j + m * h]; }
            for (int s = 0; s < S; s++) {
                const int H = h << s, d = 1 << s;
                for (int m = 0; m < R; m++) {
                    if (m & d) continue;
                    const int k = H - 1 + j + (m & (d - 1)) * h;
                    const float tr = wr[k] * xr[m + d] - wi[k] * xi[m + d];
                    const float ti = wr[k] * xi[m + d] + wi[k] * xr[m + d];
                    xr[m + d] = xr[m] - tr; xi[m + d] = xi[m] - ti;
                    xr[m] += tr; xi[m] += ti;
                }
            }
            for (int m = 0; m < R; m++) { re[i + j + m * h] = xr[m]; im[i + j + m * h] = xi[m]; }
        }
}
#endif

DECT2_SIMD_BODY void forwardBody(float* __restrict re, float* __restrict im, int log2n, int passFrom) {
    const Plan& pl = planFor(log2n);
    const int n = pl.n;
    // bit reversal as a gather from a copy (re[i] = old re[rev[i]], what the swaps of the involution give) with the first stage (half = 1,
    // no multiplications) in the same pass: the swap loop's branch (j > i) was mispredicted half the time and cost more than all the
    // butterflies
    thread_local std::vector<float> scratch;
    if (scratch.size() < 2 * (size_t)n) scratch.resize(2 * (size_t)n);
    float* __restrict cr = scratch.data();
    float* __restrict ci = cr + n;
    std::memcpy(cr, re, sizeof(float) * n);
    std::memcpy(ci, im, sizeof(float) * n);
    auto first = [&](uint32_t i, uint32_t a, uint32_t b) {
        const float ar = cr[a], ai = ci[a], br = cr[b], bi = ci[b];
        re[i] = ar + br; im[i] = ai + bi; re[i + 1] = ar - br; im[i + 1] = ai - bi;
    };
    if (log2n >= 8) {
        // in 16 x 16 tiles: i = (hi, mid, lo) reads (rev lo, rev mid, rev hi), so both the reads and the writes of a tile stay within
        // 16 cache-line runs instead of jumping across the whole array for every sample
        constexpr int B = 4;
        static constexpr uint8_t r4[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
        const int mb = log2n - 2 * B, sh = mb + B;
        const uint32_t* rv = pl.rev.data();
        for (uint32_t mid = 0; mid < (1u << mb); mid++) {
            const uint32_t rmid = rv[mid << B] >> B & ((1u << mb) - 1);   // mid's bits reversed within mb bits
            for (uint32_t hi = 0; hi < 16; hi++) {
                const uint32_t i0 = hi << sh | mid << B, s0 = rmid << B | r4[hi];
                for (uint32_t lo = 0; lo < 16; lo += 2) first(i0 | lo, (uint32_t)r4[lo] << sh | s0, (uint32_t)r4[lo + 1] << sh | s0);
            }
        }
    } else {
        const uint32_t* __restrict rv = pl.rev.data();
        for (int i = 0; i < n; i += 2) first((uint32_t)i, rv[i], rv[i + 1]);
    }
    // the stages below half-length passFrom one by one, as before; the callers run the rest several stages per pass
    for (int h = 2; h < n && h < passFrom; h <<= 1) {
        const float* __restrict wr = &pl.wr[h - 1];
        const float* __restrict wi = &pl.wi[h - 1];
        for (int i = 0; i < n; i += 2 * h) {
            float* __restrict ar = re + i;
            float* __restrict ai = im + i;
            float* __restrict br = re + i + h;
            float* __restrict bi = im + i + h;
            for (int j = 0; j < h; j++) {
                const float tr = wr[j] * br[j] - wi[j] * bi[j];
                const float ti = wr[j] * bi[j] + wi[j] * br[j];
                br[j] = ar[j] - tr; bi[j] = ai[j] - ti;
                ar[j] += tr; ai[j] += ti;
            }
        }
    }
}

#if defined(DECT2_X86_DISPATCH)
// AVX2 (clang builds): eight j at a time, from half-length 8 on (the stages 2 and 4 stay the one-stage loop). The butterfly with the
// contraction clang gives the one-stage loop in its AVX2 copy (the first product of each sum fused; tests/dsp/test_fft_bits.cpp compares
// against that loop compiled the same way, run under Rosetta with AVX2 forced on).
DECT2_AVX2_TARGET static inline void bfly8(__m256& ar, __m256& ai, __m256& br, __m256& bi, __m256 wr, __m256 wi) {
#if defined(__clang__)
    const __m256 tr = _mm256_fmsub_ps(wr, br, _mm256_mul_ps(wi, bi));
    const __m256 ti = _mm256_fmadd_ps(wr, bi, _mm256_mul_ps(wi, br));
#else
    // GCC contracts the one-stage loop's butterfly with the second product fused (seen on the x86 CI): the same here
    const __m256 tr = _mm256_fnmadd_ps(wi, bi, _mm256_mul_ps(wr, br));
    const __m256 ti = _mm256_fmadd_ps(wi, br, _mm256_mul_ps(wr, bi));
#endif
    br = _mm256_sub_ps(ar, tr); bi = _mm256_sub_ps(ai, ti);
    ar = _mm256_add_ps(ar, tr); ai = _mm256_add_ps(ai, ti);
}
template <int S>
DECT2_AVX2_TARGET static void stagesPassAvx2(float* __restrict re, float* __restrict im, int n, int h, const float* wr, const float* wi) {
    constexpr int R = 1 << S;
    for (int i = 0; i < n; i += R * h)
        for (int j = 0; j < h; j += 8) {
            __m256 xr[R], xi[R];
            for (int m = 0; m < R; m++) { xr[m] = _mm256_loadu_ps(re + i + j + m * h); xi[m] = _mm256_loadu_ps(im + i + j + m * h); }
            for (int s = 0; s < S; s++) {
                const int H = h << s, d = 1 << s;
                for (int m = 0; m < R; m++) {
                    if (m & d) continue;
                    const int k = H - 1 + j + (m & (d - 1)) * h;
                    bfly8(xr[m], xi[m], xr[m + d], xi[m + d], _mm256_loadu_ps(wr + k), _mm256_loadu_ps(wi + k));
                }
            }
            for (int m = 0; m < R; m++) { _mm256_storeu_ps(re + i + j + m * h, xr[m]); _mm256_storeu_ps(im + i + j + m * h, xi[m]); }
        }
}
DECT2_AVX2_TARGET static void forwardAvx2(float* __restrict re, float* __restrict im, int log2n) {
    // (GCC: bfly8 above follows its contraction of the one-stage loop, so the passes give that loop's bits; tests/dsp/test_fft_bits.cpp checks)
    forwardBody(re, im, log2n, 8);
    const Plan& pl = planFor(log2n);
    const int n = pl.n;
    const float* wr = pl.wr.data();
    const float* wi = pl.wi.data();
    for (int h = 8; h < n;) {
        if (8 * h <= n) { stagesPassAvx2<3>(re, im, n, h, wr, wi); h *= 8; }
        else if (4 * h <= n) { stagesPassAvx2<2>(re, im, n, h, wr, wi); h *= 4; }
        else { stagesPassAvx2<1>(re, im, n, h, wr, wi); h *= 2; }
    }
}
#endif

void forward(float* __restrict re, float* __restrict im, int log2n) {
#if defined(DECT2_X86_DISPATCH)
    if (cpuHasAvx2()) { forwardAvx2(re, im, log2n); return; }
#endif
#if !DECT2_FFT_NEON && !defined(__clang__) && (defined(__FMA__) || defined(__ARM_FEATURE_FMA))
    // GCC with fused multiply-add in the baseline (an AVX2 build, or a non-NEON FMA target): its contraction of the one-stage loop is not
    // reproduced by the plain passes (seen with GCC on arm64), so the loop stays for every stage
    forwardBody(re, im, log2n, 1 << 30);
    return;
#endif
    forwardBody(re, im, log2n, 4);
    const Plan& pl = planFor(log2n);
    const int n = pl.n;
    if (n <= 4) return;
#if DECT2_FFT_NEON
    fftStagesNeon(re, im, n, pl.wr.data(), pl.wi.data());
#else
    // the baseline x86 copy and other targets: no fused multiply-add (or clang, checked), so the passes give the loop's bits
    const float* wr = pl.wr.data();
    const float* wi = pl.wi.data();
    for (int h = 4; h < n;) {
        if (8 * h <= n) { stagesPassGeneric<3>(re, im, n, h, wr, wi); h *= 8; }
        else if (4 * h <= n) { stagesPassGeneric<2>(re, im, n, h, wr, wi); h *= 4; }
        else { stagesPassGeneric<1>(re, im, n, h, wr, wi); h *= 2; }
    }
#endif
}
} // namespace

void fftSplit(float* re, float* im, int log2n, bool inverse) {
    if (log2n < 1) return;
    if (inverse) forward(im, re, log2n);   // ifft(x) = swap(fft(swap(x))): the unnormalised inverse
    else forward(re, im, log2n);
}

DECT2_SIMD_BODY void convCorrBody(const float* a, const float* f, float* c, int strideC, int n, int p) {
    if (p <= 32) { convCorrShortBody(a, f, c, strideC, n, p); return; }
    for (int i = 0; i < n; i++) {
        float acc[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        const float* x = a + i;
        int k = 0;
        for (; k + 8 <= p; k += 8) for (int l = 0; l < 8; l++) acc[l] += f[k + l] * x[k + l];
        float s = ((acc[0] + acc[4]) + (acc[1] + acc[5])) + ((acc[2] + acc[6]) + (acc[3] + acc[7]));
        for (; k < p; k++) s += f[k] * x[k];
        c[(size_t)i * strideC] = s;
    }
}
DECT2_AVX2_DISPATCH(void, convCorr, (const float* a, const float* f, float* c, int strideC, int n, int p), (a, f, c, strideC, n, p))

DECT2_SIMD_BODY void desampBody(const float* a, int decim, const float* f, float* c, int n, int p) {
    for (int i = 0; i < n; i++) {
        float acc[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        const float* x = a + (size_t)i * decim;
        int k = 0;
        for (; k + 8 <= p; k += 8) for (int l = 0; l < 8; l++) acc[l] += f[k + l] * x[k + l];
        float s = ((acc[0] + acc[4]) + (acc[1] + acc[5])) + ((acc[2] + acc[6]) + (acc[3] + acc[7]));
        for (; k < p; k++) s += f[k] * x[k];
        c[i] = s;
    }
}
DECT2_AVX2_DISPATCH(void, desamp, (const float* a, int decim, const float* f, float* c, int n, int p), (a, decim, f, c, n, p))

void hannWindowNorm(float* w, int n) {
    const double scale = std::sqrt(8.0 / 3.0);
    for (int i = 0; i < n; i++) w[i] = (float)(scale * 0.5 * (1.0 - std::cos(2.0 * M_PI * i / n)));
}

DECT2_SIMD_BODY void syrkLowerTBody(const float* x, int rows, int n, float* a) {
    std::memset(a, 0, sizeof(float) * (size_t)n * n);
    for (int r = 0; r < rows; r++) {
        const float* __restrict xr = x + (size_t)r * n;
        for (int i = 0; i < n; i++) {
            float* __restrict row = a + (size_t)i * n;
            const float xi = xr[i];
            for (int j = 0; j <= i; j++) row[j] += xi * xr[j];   // an axpy per row: vectorises
        }
    }
}
#if defined(DECT2_X86_DISPATCH)
// AVX2 + FMA: the same sums in tiles of 4 x 16 elements of a held in registers over blocks of 256 rows (copied to a padded buffer that stays in
// the cache). Every element still adds the rows in order with one fused multiply-add each, which is what the axpy loop above compiles to in its
// AVX2 copy, so the result is the same; the axpy loop is bound by the loads and stores of a (one of each per multiply-add).
DECT2_AVX2_TARGET static void syrkLowerTAvx2(const float* x, int rows, int n, float* a) {
    std::memset(a, 0, sizeof(float) * (size_t)n * n);
    constexpr int RB = 256;
    const int np = (n + 3 + 15) & ~15;   // row length of the copy: room for the tiles past the last column and row
    thread_local std::vector<float> buf;
    for (int r0 = 0; r0 < rows; r0 += RB) {
        const int nr = std::min(RB, rows - r0);
        buf.assign((size_t)nr * np, 0.f);
        for (int r = 0; r < nr; r++) std::memcpy(&buf[(size_t)r * np], x + (size_t)(r0 + r) * n, sizeof(float) * (size_t)n);
        const float* bp = buf.data();
        for (int i0 = 0; i0 < n; i0 += 4)
            for (int j0 = 0; j0 <= i0 + 3 && j0 < n; j0 += 16) {
                alignas(32) float t[4][16];
                for (int q = 0; q < 4; q++)
                    for (int c = 0; c < 16; c++) {
                        const int i = i0 + q, j = j0 + c;
                        t[q][c] = (i < n && j <= i) ? a[(size_t)i * n + j] : 0.f;
                    }
                __m256 c00 = _mm256_load_ps(&t[0][0]), c01 = _mm256_load_ps(&t[0][8]), c10 = _mm256_load_ps(&t[1][0]), c11 = _mm256_load_ps(&t[1][8]);
                __m256 c20 = _mm256_load_ps(&t[2][0]), c21 = _mm256_load_ps(&t[2][8]), c30 = _mm256_load_ps(&t[3][0]), c31 = _mm256_load_ps(&t[3][8]);
                for (int r = 0; r < nr; r++) {
                    const float* xr = bp + (size_t)r * np;
                    const __m256 x0 = _mm256_loadu_ps(xr + j0), x1 = _mm256_loadu_ps(xr + j0 + 8);
                    const __m256 y0 = _mm256_broadcast_ss(xr + i0), y1 = _mm256_broadcast_ss(xr + i0 + 1);
                    const __m256 y2 = _mm256_broadcast_ss(xr + i0 + 2), y3 = _mm256_broadcast_ss(xr + i0 + 3);
                    c00 = _mm256_fmadd_ps(y0, x0, c00); c01 = _mm256_fmadd_ps(y0, x1, c01);
                    c10 = _mm256_fmadd_ps(y1, x0, c10); c11 = _mm256_fmadd_ps(y1, x1, c11);
                    c20 = _mm256_fmadd_ps(y2, x0, c20); c21 = _mm256_fmadd_ps(y2, x1, c21);
                    c30 = _mm256_fmadd_ps(y3, x0, c30); c31 = _mm256_fmadd_ps(y3, x1, c31);
                }
                _mm256_store_ps(&t[0][0], c00); _mm256_store_ps(&t[0][8], c01); _mm256_store_ps(&t[1][0], c10); _mm256_store_ps(&t[1][8], c11);
                _mm256_store_ps(&t[2][0], c20); _mm256_store_ps(&t[2][8], c21); _mm256_store_ps(&t[3][0], c30); _mm256_store_ps(&t[3][8], c31);
                for (int q = 0; q < 4; q++)
                    for (int c = 0; c < 16; c++) {
                        const int i = i0 + q, j = j0 + c;
                        if (i < n && j <= i) a[(size_t)i * n + j] = t[q][c];
                    }
            }
    }
}
void syrkLowerT(const float* x, int rows, int n, float* a) {
    if (cpuHasAvx2()) { syrkLowerTAvx2(x, rows, n, a); return; }
    syrkLowerTBody(x, rows, n, a);
}
#else
DECT2_AVX2_DISPATCH(void, syrkLowerT, (const float* x, int rows, int n, float* a), (x, rows, n, a))
#endif

DECT2_SIMD_BODY void gemvTBody(const float* x, int rows, int n, const float* t, float* b) {
    std::memset(b, 0, sizeof(float) * n);
    for (int r = 0; r < rows; r++) {
        const float* __restrict xr = x + (size_t)r * n;
        const float tr = t[r];
        for (int i = 0; i < n; i++) b[i] += tr * xr[i];
    }
}
DECT2_AVX2_DISPATCH(void, gemvT, (const float* x, int rows, int n, const float* t, float* b), (x, rows, n, t, b))

#endif

} // namespace dect2
