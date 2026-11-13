// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/stage_prof.h"
#include "dect2/dsp/simd.h"
#if defined(DECT2_X86_DISPATCH)
#include <immintrin.h>
#endif
#include "dect2/common/thread_name.h"
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
#include <arm_neon.h>
#endif
#include "dect2/atsc3/atsc3_sync.h"
#include "dect2/dsp/dsp_compat.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <map>
#include <thread>
#include <memory>
#include <mutex>
#include "dect2/common/window.h"
#include <cstdlib>
#include <cstring>

namespace dect2 {
namespace atsc3 {

namespace {
const double kFs = kBootstrapRate;

cf32 expj(double ph) { return cf32((float)std::cos(ph), (float)std::sin(ph)); }
}

namespace {
struct Bank {
    // 512 phases: the nearest one is within 1/1024 of a sample, an error of about -65 dB over a 6 MHz channel (-70 dB on a 0.9 MHz tone at
    // 10 Msps), far under the receiver's own (about -33 dB at the decoder on a real capture). 2048 phases (a 400 kB bank, a random row of it for
    // every output) cost a third more time in cache misses for nothing measurable: same transport stream.
    int half = 16, taps = 32, phases = 512, stride = 32;   // stride: taps rounded up to a multiple of four, the extra coefficients are zero
    double fc = 0.5;
    std::vector<float> h;   // (phases + 1) x taps
    // the same rows in the shared-window layout: shift zeros, the taps, zeros up to padL; four outputs whose windows start within `shift` samples of
    // the first one's read padW samples of one window, output u from offset shift - s_u of its row (empty: not used)
    int shift = 0, padW = 0, padL = 0;
    std::vector<float> hp;
};

double besselI0(double x) { return dect2::window::besselI0(x, 30, 0.0); }

Bank makeBank(double ratio) {
    Bank b;
    const double down = std::min(1.0, ratio);
    b.fc = 0.5 * down * 0.97;
    b.half = (int)std::ceil(16.0 / down);
    b.taps = 2 * b.half;
    b.stride = (b.taps + 7) & ~7;
    b.h.assign((size_t)(b.phases + 1) * b.stride, 0.f);
    const double beta = 8.0, i0b = besselI0(beta);
    for (int p = 0; p <= b.phases; p++) {
        double tau = (double)p / b.phases, sum = 0;
        std::vector<double> t(b.taps);
        for (int k = 0; k < b.taps; k++) {
            double x = (k - (b.half - 1)) - tau;   // distance of input sample (floor + k - half + 1) from the output position
            double w = std::fabs(x) < b.half ? besselI0(beta * std::sqrt(1.0 - (x / b.half) * (x / b.half))) / i0b : 0.0;
            double sc = x == 0 ? 1.0 : std::sin(2 * M_PI * b.fc * x) / (2 * M_PI * b.fc * x);
            t[k] = 2 * b.fc * sc * w;
            sum += t[k];
        }
        for (int k = 0; k < b.taps; k++) b.h[(size_t)p * b.stride + k] = (float)(t[k] / sum);
    }
    // four outputs span three steps of 1 / ratio input samples (the nearest phase may be the row after the last, never the next sample)
    b.shift = (int)std::ceil(3.0 / ratio) + 1;
    if (b.shift <= 16) {
        b.padW = (b.taps + b.shift + 7) & ~7;
        b.padL = (b.shift + b.padW + 3) & ~3;
        b.hp.assign((size_t)(b.phases + 1) * b.padL, 0.f);
        for (int p = 0; p <= b.phases; p++)
            for (int k = 0; k < b.taps; k++) b.hp[(size_t)p * b.padL + b.shift + k] = b.h[(size_t)p * b.stride + k];
    }
    return b;
}
}

#ifdef DECT2_X86_DISPATCH
// Four outputs at once, AVX2 + FMA (eight independent accumulator chains)
DECT2_AVX2_TARGET static void dot4Avx2(const float* const* h, const float* const* r, const float* const* m, int taps, cf32* o) {
    __m256 ar[4], ai[4];
    for (int u = 0; u < 4; u++) ar[u] = ai[u] = _mm256_setzero_ps();
    for (int t = 0; t < taps; t += 8)
        for (int u = 0; u < 4; u++) {
            const __m256 hv = _mm256_loadu_ps(h[u] + t);
            ar[u] = _mm256_fmadd_ps(_mm256_loadu_ps(r[u] + t), hv, ar[u]);
            ai[u] = _mm256_fmadd_ps(_mm256_loadu_ps(m[u] + t), hv, ai[u]);
        }
    for (int u = 0; u < 4; u++) {
        __m128 a = _mm_add_ps(_mm256_castps256_ps128(ar[u]), _mm256_extractf128_ps(ar[u], 1)), b = _mm_add_ps(_mm256_castps256_ps128(ai[u]), _mm256_extractf128_ps(ai[u], 1));
        a = _mm_hadd_ps(a, b); a = _mm_hadd_ps(a, a);
        o[u] = cf32(_mm_cvtss_f32(a), _mm_cvtss_f32(_mm_shuffle_ps(a, a, 1)));
    }
}
// Four outputs from one shared window (the layout of Bank::hp), eight lanes: each data vector is loaded once for the four outputs, and their sums
// across lanes are formed together (two rounds of hadd) instead of one reduction per output. Returns where it stopped.
DECT2_AVX2_TARGET static long resPadAvx2(const Bank& b, const float* re, const float* im, long pad, double startOffset, double step, long k, long k1, cf32* out) {
    const int W = b.padW, L = b.padL, S = b.shift;
    for (; k + 4 <= k1; k += 4) {
        long i[4]; int p0[4];
        for (int u = 0; u < 4; u++) {
            const double pos = startOffset + (double)(k + u) * step;
            i[u] = (long)std::floor(pos);   // one instruction here (the target has SSE4.1)
            p0[u] = (int)iround((pos - (double)i[u]) * b.phases);
        }
        const float* hq[4];
        bool fits = true;
        for (int u = 0; u < 4; u++) {
            const long sh = i[u] - i[0];
            fits = fits && sh >= 0 && sh <= S;
            hq[u] = &b.hp[(size_t)p0[u] * L + (size_t)(S - std::min<long>(std::max<long>(sh, 0), S))];
        }
        if (!fits) break;
        const float* r = &re[(size_t)(i[0] - (b.half - 1) + pad)];
        const float* m = &im[(size_t)(i[0] - (b.half - 1) + pad)];
        __m256 ar[4], ai[4];
        for (int u = 0; u < 4; u++) ar[u] = ai[u] = _mm256_setzero_ps();
        for (int t = 0; t < W; t += 8) {
            const __m256 rv = _mm256_loadu_ps(r + t), mv = _mm256_loadu_ps(m + t);
            for (int u = 0; u < 4; u++) {
                const __m256 hv = _mm256_loadu_ps(hq[u] + t);
                ar[u] = _mm256_fmadd_ps(rv, hv, ar[u]);
                ai[u] = _mm256_fmadd_ps(mv, hv, ai[u]);
            }
        }
        const __m256 hr = _mm256_hadd_ps(_mm256_hadd_ps(ar[0], ar[1]), _mm256_hadd_ps(ar[2], ar[3]));
        const __m256 hi = _mm256_hadd_ps(_mm256_hadd_ps(ai[0], ai[1]), _mm256_hadd_ps(ai[2], ai[3]));
        const __m128 sr = _mm_add_ps(_mm256_castps256_ps128(hr), _mm256_extractf128_ps(hr, 1));   // re of outputs 0..3
        const __m128 si = _mm_add_ps(_mm256_castps256_ps128(hi), _mm256_extractf128_ps(hi, 1));
        float* o = reinterpret_cast<float*>(out + k);
        _mm_storeu_ps(o, _mm_unpacklo_ps(sr, si));
        _mm_storeu_ps(o + 4, _mm_unpackhi_ps(sr, si));
    }
    return k;
}
#endif

namespace {
// x[i] * exp(j w (i + start)), w = -2 pi f / rate, written split (re, im) or back in place (re == nullptr). The phase is exact (double) at the start
// of every block of 256 samples and inside a block comes from a table of exp(j w k): no chain of products, so the loop vectorises; the error is a few
// 1e-7 rad (the former running product in double, converted to float per sample, was no better than float either).
void rotate(cf32* xio, const cf32* x, size_t n, double f, double rate, double start, float* re, float* im) {
    constexpr int kB = 256;
    const double w = -2.0 * M_PI * f / rate;
    float tr[kB], ti[kB];
    for (int k = 0; k < kB; k++) { tr[k] = (float)std::cos(w * k); ti[k] = (float)std::sin(w * k); }
    for (size_t i0 = 0; i0 < n; i0 += kB) {
        const double a = std::fmod(w * (start + (double)i0), 2.0 * M_PI);
        const float cr = (float)std::cos(a), ci = (float)std::sin(a);
        const size_t e = std::min<size_t>(kB, n - i0);
        const float* xp = reinterpret_cast<const float*>(x + i0);
        if (re) {
            float* __restrict orr = re + i0;
            float* __restrict oi = im + i0;
            for (size_t k = 0; k < e; k++) {
                const float pr = cr * tr[k] - ci * ti[k], pi = cr * ti[k] + ci * tr[k], xr = xp[2 * k], xi = xp[2 * k + 1];
                orr[k] = xr * pr - xi * pi; oi[k] = xr * pi + xi * pr;
            }
        } else {
            float* o = reinterpret_cast<float*>(xio + i0);   // may be x itself: no restrict here
            for (size_t k = 0; k < e; k++) {
                const float pr = cr * tr[k] - ci * ti[k], pi = cr * ti[k] + ci * tr[k], xr = xp[2 * k], xi = xp[2 * k + 1];
                o[2 * k] = xr * pr - xi * pi; o[2 * k + 1] = xr * pi + xi * pr;
            }
        }
    }
}
void resampleImpl(const cf32* in, size_t n, double inRate, double outRate, std::vector<cf32>& out, double startOffset, double rotHz, double rotStart);
}

void resampleExact(const cf32* in, size_t n, double inRate, double outRate, std::vector<cf32>& out, double startOffset) {
    resampleImpl(in, n, inRate, outRate, out, startOffset, 0.0, 0.0);
}
void resampleExactRot(const cf32* in, size_t n, double inRate, double outRate, std::vector<cf32>& out, double rotHz, double rotStart, double startOffset) {
    resampleImpl(in, n, inRate, outRate, out, startOffset, rotHz, rotStart);
}

namespace {
void resampleImpl(const cf32* in, size_t n, double inRate, double outRate, std::vector<cf32>& out, double startOffset, double rotHz, double rotStart) {
    DECT2_STAGE("a3.resample");
    out.clear();
    if (n == 0) return;
    const double ratio = outRate / inRate;
    // the filter bank depends on the ratio only and is costly to design: keep the last few
    static std::mutex bmu;
    static std::vector<std::pair<double, std::shared_ptr<const Bank>>> bankCache;
    std::shared_ptr<const Bank> bp;
    {
        std::lock_guard<std::mutex> lk(bmu);
        for (auto& e : bankCache) if (e.first == ratio) bp = e.second;
        if (!bp) {
            bp = std::make_shared<const Bank>(makeBank(ratio));
            if (bankCache.size() >= 8) bankCache.erase(bankCache.begin());
            bankCache.push_back({ratio, bp});
        }
    }
    const Bank& b = *bp;
    const double step = inRate / outRate;
    const long count = (long)std::floor(((double)n - 1 - startOffset) / step) + 1;
    if (count <= 0) return;
    // split input with zero padding on both sides, so that the inner loop has no bounds checks
    const long pad = b.stride + 2 + b.shift + 8;   // room for the shared windows' longer reads too
    std::vector<float> re((size_t)n + 2 * pad, 0.f), im((size_t)n + 2 * pad, 0.f);
    if (rotHz != 0.0) rotate(nullptr, in, n, rotHz, inRate, rotStart, &re[(size_t)pad], &im[(size_t)pad]);
    else for (size_t i = 0; i < n; i++) { re[i + pad] = in[i].real(); im[i + pad] = in[i].imag(); }
    out.resize((size_t)count);
    const int taps = b.stride;
    // where output k reads: the input rows from base and the tap row of its nearest phase
    auto locate = [&](long k, const float*& h, const float*& r, const float*& m) {
        const double pos = startOffset + (double)k * step;
        const long i = (long)ifloor(pos);   // no libm call on the x86 baseline (dect2/dsp/simd.h); the same values
        const int p0 = (int)iround((pos - (double)i) * b.phases);   // nearest phase of the bank: a timing error below 1/1000 of a sample
        h = &b.h[(size_t)p0 * taps];
        const long base = i - (b.half - 1) + pad;
        r = &re[(size_t)base];
        m = &im[(size_t)base];
    };
    auto part = [&](long k0, long k1) {
        DECT2_STAGE("a3.resample");
        long k = k0;
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
        // Four outputs from one shared window of input samples (each output's taps at its offset in a zero-padded row): the data are loaded once
        // for all four and each tap vector feeds two multiply-adds, where one window per output loads three vectors for two (the loop below is
        // load-bound). Two partial sums per row and output: sixteen independent chains. (x86 keeps its AVX2 path, eight lanes wide.)
        static const bool noPad = getenv("ATSC3_NOPAD") != nullptr;   // test: one window per output
        if (!b.hp.empty() && !noPad) {
            typedef float v4 __attribute__((vector_size(16)));
            auto ld = [](const float* q) { v4 v; std::memcpy(&v, q, 16); return v; };
            const int W = b.padW, L = b.padL, S = b.shift;
            for (; k + 4 <= k1; k += 4) {
                long i[4]; int p0[4];
                for (int u = 0; u < 4; u++) {
                    const double pos = startOffset + (double)(k + u) * step;
                    i[u] = (long)std::floor(pos);
                    p0[u] = (int)std::lround((pos - (double)i[u]) * b.phases);
                }
                const float* hq[4];
                bool fits = true;
                for (int u = 0; u < 4; u++) {
                    const long sh = i[u] - i[0];
                    fits = fits && sh >= 0 && sh <= S;
                    hq[u] = &b.hp[(size_t)p0[u] * L + (size_t)(S - std::min<long>(std::max<long>(sh, 0), S))];
                }
                if (!fits) break;
                const float* r = &re[(size_t)(i[0] - (b.half - 1) + pad)];
                const float* m = &im[(size_t)(i[0] - (b.half - 1) + pad)];
                v4 ar[4] = {}, ai[4] = {}, br[4] = {}, bi[4] = {};
                for (int t = 0; t < W; t += 8) {
                    const v4 r0 = ld(r + t), m0 = ld(m + t), r1 = ld(r + t + 4), m1 = ld(m + t + 4);
                    for (int u = 0; u < 4; u++) {
                        const v4 h0 = ld(hq[u] + t), h1 = ld(hq[u] + t + 4);
                        ar[u] += r0 * h0; ai[u] += m0 * h0;
                        br[u] += r1 * h1; bi[u] += m1 * h1;
                    }
                }
                for (int u = 0; u < 4; u++) {
                    const v4 sr = ar[u] + br[u], si = ai[u] + bi[u];
                    out[(size_t)(k + u)] = cf32((sr[0] + sr[1]) + (sr[2] + sr[3]), (si[0] + si[1]) + (si[2] + si[3]));
                }
            }
        }
#endif
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
        // Four outputs at once: each keeps the four partial sums of the scalar loop below (lane j adds taps j, j + 4, ... in order, lanes joined
        // (0 + 1) + (2 + 3)); the four independent chains hide the latency of one output's chain of dependent additions. Fused multiply-adds
        // (as the AVX2 path): half the instructions, and a few ulp from the scalar loop, which rounds the product first.
        for (; k + 4 <= k1; k += 4) {
            const float *h[4], *r[4], *m[4];
            float32x4_t ar[4], ai[4];
            for (int u = 0; u < 4; u++) { locate(k + u, h[u], r[u], m[u]); ar[u] = ai[u] = vdupq_n_f32(0); }
            for (int t = 0; t < taps; t += 4)
                for (int u = 0; u < 4; u++) {
                    const float32x4_t hv = vld1q_f32(h[u] + t);
                    ar[u] = vfmaq_f32(ar[u], vld1q_f32(r[u] + t), hv);
                    ai[u] = vfmaq_f32(ai[u], vld1q_f32(m[u] + t), hv);
                }
            for (int u = 0; u < 4; u++) {
                const float32x4_t s = vpaddq_f32(ar[u], ai[u]);   // (re0+re1) (re2+re3) (im0+im1) (im2+im3)
                out[(size_t)(k + u)] = cf32(vgetq_lane_f32(s, 0) + vgetq_lane_f32(s, 1), vgetq_lane_f32(s, 2) + vgetq_lane_f32(s, 3));
            }
        }
#endif
#ifdef DECT2_X86_DISPATCH
        if (cpuHasAvx2()) {
            static const bool noPadX = getenv("ATSC3_NOPAD") != nullptr;   // test: one window per output
            if (!b.hp.empty() && !noPadX) k = resPadAvx2(b, re.data(), im.data(), pad, startOffset, step, k, k1, out.data());
            for (; k + 4 <= k1; k += 4) {
                const float *h[4], *r[4], *m[4];
                for (int u = 0; u < 4; u++) locate(k + u, h[u], r[u], m[u]);
                dot4Avx2(h, r, m, taps, &out[(size_t)k]);
            }
        }
#endif
        for (; k < k1; k++) {
            const float *h, *r, *m;
            locate(k, h, r, m);
            float ar[4] = {0, 0, 0, 0}, ai[4] = {0, 0, 0, 0};
            for (int t = 0; t < taps; t += 4)
                for (int j = 0; j < 4; j++) { ar[j] += r[t + j] * h[t + j]; ai[j] += m[t + j] * h[t + j]; }
            out[(size_t)k] = cf32((ar[0] + ar[1]) + (ar[2] + ar[3]), (ai[0] + ai[1]) + (ai[2] + ai[3]));
        }
    };
    // Long blocks are split over a few threads (the outputs do not depend on each other), but only while one thread would spend more than about
    // a third of real time here (a slow processor): otherwise the threads started per block cost more CPU than they save, and on a processor with
    // two threads per core both run slower side by side. The load is the resampler's CPU time per second of input, averaged over all calls.
    static std::atomic<float> load{0.f};
    static const int forceSplit = getenv("ATSC3_SPLIT") ? atoi(getenv("ATSC3_SPLIT")) : -1;   // test: 1 always split, 0 never
    const int hw = (int)std::thread::hardware_concurrency();
    const float ld = load.load(std::memory_order_relaxed);
    const bool want = forceSplit >= 0 ? forceSplit > 0 : ld > 0.33f;
    // the parts stay the same whether they run on threads or one after the other here (each groups its outputs from its own start: the same bits)
    const int parts = (count >= 200000 && hw >= 4) ? std::min(4, hw / 2) : 1;
    const int nt = want ? parts : 1;
    const double secs = (double)n / inRate;
    const uint64_t c0 = prof::threadCpuNs();
    if (nt <= 1) {
        for (int t = 0; t < parts; t++) part(count * t / parts, count * (t + 1) / parts);
        const double w = (double)(prof::threadCpuNs() - c0) * 1e-9;
        if (secs > 0) load.store(ld + 0.2f * ((float)std::min(w / secs, 4.0) - ld), std::memory_order_relaxed);
        return;
    }
    std::vector<std::thread> th;
    std::atomic<uint64_t> helperNs{0};
    auto timedPart = [&](long a, long e) { const uint64_t h0 = prof::threadCpuNs(); part(a, e); helperNs += prof::threadCpuNs() - h0; };
    for (int t = 1; t < nt; t++) th.emplace_back(dect2::namedThread("onair-a3sync", timedPart, count * t / nt, count * (t + 1) / nt));
    part(0, count / nt);
    for (auto& t : th) t.join();
    const double w = ((double)(prof::threadCpuNs() - c0) + (double)helperNs.load()) * 1e-9;
    if (secs > 0) load.store(ld + 0.2f * ((float)std::min(w / secs, 4.0) - ld), std::memory_order_relaxed);
}

} // namespace

void derotate(cf32* x, size_t n, double f, double rate, double start) {
    DECT2_STAGE("a3.derotate");
    if (n) rotate(x, x, n, f, rate, start, nullptr, nullptr);
}

double estimateCfoGuard(const cf32* x, size_t n, int fft, int guard, int symbols, double rate) {
    std::complex<double> acc(0, 0);
    const size_t sl = (size_t)fft + guard;
    for (int s = 0; s < symbols; s++) {
        size_t b = (size_t)s * sl;
        if (b + sl > n) break;
        for (int i = 0; i < guard; i++) acc += std::complex<double>(x[b + i]) * std::conj(std::complex<double>(x[b + fft + i]));
    }
    // x[n + fft] = x[n] e^{j 2 pi f fft / rate}, so the sum of x[n] conj(x[n + fft]) has the phase -2 pi f fft / rate
    return -std::arg(acc) * rate / (2.0 * M_PI * fft);
}

BootstrapFind findBootstrap(const cf32* x, size_t n, double maxCfo, double center, int seedOnly) {
    if (center != 0.0) {   // a known approximate offset: remove it first and search around zero
        std::vector<cf32> y(x, x + n);
        derotate(y.data(), y.size(), center, kFs, 0.0);
        BootstrapFind r = findBootstrap(y.data(), y.size(), maxCfo, 0.0, seedOnly);
        r.cfoHz += center;
        return r;
    }
    BootstrapFind res;
    const int L = kBootstrapSymbolLen;
    if (n < (size_t)5 * L) return res;
    // 1. autocorrelation at lag 2048 over 520 samples: the part C repeats the end of part A, whatever the carrier offset is
    const int lag = 2048, win = 520;
    const size_t m = n - lag;
    std::vector<std::complex<double>> pre(m + 1);
    std::vector<double> e1(m + 1), e2(m + 1);
    pre[0] = 0; e1[0] = e2[0] = 0;
    for (size_t i = 0; i < m; i++) {
        pre[i + 1] = pre[i] + std::complex<double>(x[i]) * std::conj(std::complex<double>(x[i + lag]));
        e1[i + 1] = e1[i] + std::norm(std::complex<double>(x[i]));
        e2[i + 1] = e2[i] + std::norm(std::complex<double>(x[i + lag]));
    }
    std::vector<double> met(m - win + 1);
#if defined(__x86_64__) || defined(_M_X64)
    // x86: the metric squared (the threshold too): only its order and the threshold are used, and std::abs of a complex is a hypot() call in
    // libm there, the largest part of the search's time
    constexpr bool kSquared = true;
#else
    constexpr bool kSquared = false;
#endif
    const double metThr = kSquared ? 0.35 * 0.35 : 0.35;
    for (size_t p = 0; p + win <= m; p++) {
        double a = e1[p + win] - e1[p], b = e2[p + win] - e2[p];
        if (kSquared) met[p] = (a > 0 && b > 0) ? std::norm(pre[p + win] - pre[p]) / (a * b) : 0.0;
        else met[p] = (a > 0 && b > 0) ? std::abs(pre[p + win] - pre[p]) / std::sqrt(a * b) : 0.0;
    }
    // 2. the strongest peaks, at least one symbol apart
    std::vector<std::pair<double, size_t>> cand;
    {
        std::vector<std::pair<double, size_t>> all;
        for (size_t p = 0; p < met.size(); p++) if (met[p] > metThr && (p == 0 || met[p] >= met[p - 1]) && (p + 1 == met.size() || met[p] > met[p + 1])) all.push_back({met[p], p});
        std::sort(all.rbegin(), all.rend());
        for (auto& c : all) {
            bool tooClose = false;
            for (auto& k : cand) if (std::llabs((long long)k.second - (long long)c.second) < 1500) tooClose = true;
            if (!tooClose) cand.push_back(c);
            if (cand.size() >= 5) break;
        }
    }
    // 3. for each peak: carrier offsets (fractional from the phase, plus whole carrier spacings), then the template correlation
    double bestMetric = 0;
    for (auto& cd : cand) {
        const size_t pa = cd.second;
        std::complex<double> z = pre[pa + win] - pre[pa];
        const double fFrac = -std::arg(z) * kFs / (2.0 * M_PI * lag);
        const int mMax = (int)std::ceil(maxCfo / 3000.0);
        // the window: 4 symbols plus margins on both sides
        const long w0 = (long)pa - 40;
        if (w0 < 0 || (size_t)(w0 + 4 * L + 100) > n) continue;
        // the hypotheses do not depend on each other: a wide search tries them on a few threads
        std::vector<double> fs;
        for (int mi = -mMax; mi <= mMax; mi++) if (std::fabs(fFrac + mi * 3000.0) <= maxCfo + 1500) fs.push_back(fFrac + mi * 3000.0);
        std::vector<Detection> dets(fs.size());
        auto test = [&](size_t k) {
            std::vector<cf32> seg(x + w0, x + std::min(n, (size_t)(w0 + 8 * L + 100)));
            derotate(seg.data(), seg.size(), fs[k], kFs, (double)w0);
            // quick test of the hypothesis: the correlation with the first symbol over a few lags and the eight seeds
            dets[k] = detectBootstrap(seg.data(), seg.size(), seedOnly);
        };
        const int hw = (int)std::thread::hardware_concurrency();
        const size_t nt = fs.size() > 3 ? (size_t)std::max(1, std::min(8, hw)) : 1;
        if (nt <= 1) {
            for (size_t k = 0; k < fs.size(); k++) test(k);
        } else {
            std::atomic<size_t> next{0};
            auto work = [&] { for (size_t k; (k = next++) < fs.size();) test(k); };
            std::vector<std::thread> th;
            for (size_t t = 1; t < std::min(nt, fs.size()); t++) th.emplace_back(dect2::namedThread("onair-a3sync", work));
            work();
            for (auto& t : th) t.join();
        }
        for (size_t k = 0; k < fs.size(); k++) {
            const Detection& det = dets[k];
            if (!det.found || det.metric < 0.30f) continue;
            if (det.metric > bestMetric) {
                bestMetric = det.metric;
                res.found = true;
                res.start = (double)w0 + (double)det.start;
                res.cfoHz = fs[k];
                res.metric = det.metric;
                res.det = det;
            }
        }
        if (res.found && res.det.valid && res.metric > 0.6f) break;
    }
    if (!res.found) return res;
    // 4. refine the offset from the residual phase slope over the first symbol: compare the two halves
    {
        const long b = (long)std::llround(res.start);
        if (b >= 0 && (size_t)(b + 4 * L) <= n) {
            std::vector<cf32> seg(x + b, x + b + 4 * L);
            derotate(seg.data(), seg.size(), res.cfoHz, kFs, (double)b);
            auto sym = generateBootstrap(res.det.info);
            std::complex<double> c1(0, 0), c2(0, 0);
            const int half = 2 * L;   // the first two symbols against the first two of the template
            for (int i = 0; i < half; i++) c1 += std::complex<double>(seg[i]) * std::conj(std::complex<double>(sym[i]));
            for (int i = half; i < 4 * L && i < (int)sym.size(); i++) c2 += std::complex<double>(seg[i]) * std::conj(std::complex<double>(sym[i]));
            // symbols 2 and 3 carry the signalling shifts and are not identical to the template when the template is wrong, but with the right fields they are
            if (res.det.valid && std::abs(c1) > 0 && std::abs(c2) > 0) {
                double dphi = std::arg(c2 * std::conj(c1));
                res.cfoHz += dphi * kFs / (2.0 * M_PI * half);
            }
        }
    }
    return res;
}

// ---- streaming


// ---- frame decoding on worker threads

struct Atsc3Sync::Pool {
    struct Job { long seq; std::vector<cf32> y; Bootstrap bs; };
    struct Done { FrameResult fr; Bootstrap bs; };
    Atsc3Receiver* rx;
    std::mutex m;
    std::condition_variable cv, cvSpace;
    std::deque<Job> jobs;
    std::map<long, Done> done;
    long nextSeq = 0, nextCommit = 0;
    int inflight = 0, maxInflight = 2;
    bool committing = false, stop = false;
    double secDecode = 0;
    std::vector<std::thread> th;

    Pool(Atsc3Receiver* r, int n) : rx(r), maxInflight(2 * n) {
        for (int i = 0; i < n; i++) th.emplace_back(dect2::namedThread("onair-a3sync", [this] { run(); }));
    }
    ~Pool() {
        flush();
        { std::lock_guard<std::mutex> lk(m); stop = true; }
        cv.notify_all();
        for (auto& t : th) t.join();
    }
    void submit(std::vector<cf32>&& y, const Bootstrap& bs) {
        std::unique_lock<std::mutex> lk(m);
        cvSpace.wait(lk, [&] { return inflight < maxInflight; });
        jobs.push_back({nextSeq++, std::move(y), bs});
        inflight++;
        cv.notify_one();
    }
    void flush() {
        std::unique_lock<std::mutex> lk(m);
        cvSpace.wait(lk, [&] { return inflight == 0; });
    }
    void run() {
        DenormalsOff ftz;
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [&] { return stop || !jobs.empty(); });
                if (jobs.empty()) return;
                j = std::move(jobs.front());
                jobs.pop_front();
            }
            const auto t0 = std::chrono::steady_clock::now();
            DECT2_STAGE_VAR(stFr, "a3.frame");
            Done d{decodeFrame(j.y.data(), j.y.size(), j.bs), j.bs};
            stFr.stop();
            std::unique_lock<std::mutex> lk(m);
            secDecode += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            done.emplace(j.seq, std::move(d));
            if (committing) continue;   // the thread that is committing carries on with this one
            committing = true;
            while (!done.empty() && done.begin()->first == nextCommit) {
                Done c = std::move(done.begin()->second);
                done.erase(done.begin());
                nextCommit++;
                lk.unlock();
                { DECT2_STAGE("a3.upper"); rx->commitFrame(c.fr, c.bs); }
                lk.lock();
                inflight--;
                cvSpace.notify_all();
            }
            committing = false;
        }
    }
};

Atsc3Sync::Atsc3Sync(double rate, Atsc3Receiver* rx) : rate_(rate), rx_(rx) {}
Atsc3Sync::~Atsc3Sync() = default;

void Atsc3Sync::setThreads(int n) {
    pool_.reset();
    if (n > 1) pool_.reset(new Pool(rx_, n));
}

void Atsc3Sync::flush() { if (pool_) pool_->flush(); }

SyncStats Atsc3Sync::stats() const {
    SyncStats s = st_;
    if (pool_) { std::lock_guard<std::mutex> lk(pool_->m); s.secDecode = pool_->secDecode; }
    return s;
}

void Atsc3Sync::push(const cf32* x, size_t n) {
    raw_.insert(raw_.end(), x, x + n);
    work();
}

namespace {
double secondsToNext(const Bootstrap& b) { return minTimeToNextMs(b.minTimeToNext) / 1000.0; }
}

bool Atsc3Sync::search() {
    DECT2_STAGE("a3.search");
    const auto t0 = std::chrono::steady_clock::now();
    struct Timer { double& d; std::chrono::steady_clock::time_point t; ~Timer() { d += std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count(); } } tm{st_.secSearch, t0};
    // look at everything available beyond the search start (a few frames long at most), converted to the bootstrap rate
    const long from = std::max(searchFrom_, rawStart_);
    const long avail = rawStart_ + (long)raw_.size() - from;
    const double rate = rateEff();
    const long need = (long)(rate * (8.0 * 3072 / kBootstrapRate + 0.003));
    if (avail < need * 2) return false;
    // with a hint (the previous frame) a short window is enough; without one, look at a quarter of a second
    const long take = std::min<long>(avail, (long)(rate * (haveHint_ ? 0.01 : 0.25)));
    // the expected offset is removed at the radio's rate, before the band is cut to the bootstrap's: a channel recorded off centre would
    // otherwise lose an edge in the resampler. Without a hint the centre comes from the spectrum.
    std::vector<cf32> seg(raw_.begin() + (from - rawStart_), raw_.begin() + (from - rawStart_) + take);
    if (!haveHint_) coarse_ = centreOfChannel(seg.data(), seg.size());
    // the spectrum's centre can be off the true one (a radio filter that cuts into one edge of the channel moves it by a hundred kHz): without a
    // hint the search also tries the middle of the band when the centre it found gives nothing
    double pre = haveHint_ ? cfo_ : coarse_;
    BootstrapFind bf;
    for (int attempt = 0; attempt < 2; attempt++) {
        std::vector<cf32> y;
        resampleExactRot(seg.data(), seg.size(), rate, kBootstrapRate, y, pre, (double)from);
        if (attempt == 0) st_.searches++;
        bf = haveHint_ ? findBootstrap(y.data(), y.size(), 2500.0, 0.0, bs_.minorVersion) : findBootstrap(y.data(), y.size(), 20000.0);
        if (bf.found && bf.det.valid) break;
        if (haveHint_ || attempt > 0 || std::fabs(coarse_) < 15000.0) break;
        pre = 0.0;
    }
    bf.cfoHz += pre;
    if (haveHint_ && !bf.found && ++hintMisses_ > 100) { haveHint_ = false; hintMisses_ = 0; sroPpm_ = 0; sroCount_ = 0; predNext_ = -1; }   // lost: search widely again
    if (!bf.found || !bf.det.valid) {
        // the bootstrap may straddle the end: keep an overlap of five symbols (a detection needs four after its start)
        searchFrom_ = from + take - (long)(rate * 5.0 * 3072 / kBootstrapRate);
        return take == avail ? false : true;
    }
    bs_ = bf.det.info;
    cfo_ = bf.cfoHz;
    const double bsExact = (double)from + bf.start * rate / kBootstrapRate;
    bsStart_ = (long)std::llround(bsExact);
    // the clock: the previous frame said where this bootstrap starts; the difference over the frame's length is the clock error left
    if (predNext_ > 0 && predFrom_ >= 0 && predNext_ > predFrom_) {
        const double ppm = (bsExact - predNext_) / (predNext_ - predFrom_) * 1e6;
        if (std::fabs(ppm) < 400.0) {   // more: not the frame that was predicted (a gap, lost samples, a frame not read)
            sroCount_++;
            sroPpm_ += (sroCount_ < 4 ? 0.9 : 0.3) * ppm;
            sroPpm_ = std::max(-300.0, std::min(300.0, sroPpm_));
        }
    }
    predNext_ = -1;
    predFrom_ = bsExact;
    bsExact_ = bsExact;
    const double bsLen = bs_.numSymbols * 3072 / kBootstrapRate;
    frameStart_ = bsStart_ + (long)std::llround(bsLen * rateEff());
    minToNextSec_ = secondsToNext(bs_);
    locked_ = true;
    haveHint_ = true;
    frameSamples_ = 0;
    st_.bootstraps++;
    st_.locked = true;
    st_.cfoHz = cfo_;
    st_.sroPpm = sroPpm_;
    return true;
}

// The centre of the strongest 5.83 MHz wide block of the spectrum (an ATSC 3.0 channel of 6 MHz, the bootstrap's band and more), in Hz
// from the middle of the sample band; 0 when nothing stands out.
double Atsc3Sync::centreOfChannel(const cf32* x, size_t n) const {
    const int lg = 10, N = 1 << lg;
    if (n < (size_t)N * 8) return 0.0;
    std::vector<double> P((size_t)N, 0.0);
    std::vector<float> re((size_t)N), im((size_t)N);
    const size_t blocks = std::min<size_t>(n / N, 256), stride = n / blocks;
    for (size_t b = 0; b < blocks; b++) {
        const cf32* p = x + b * stride;
        bool finite = true;
        for (int i = 0; i < N; i++) {
            const float w = 0.5f - 0.5f * std::cos(2.0f * (float)M_PI * (i + 0.5f) / N);
            re[(size_t)i] = p[i].real() * w; im[(size_t)i] = p[i].imag() * w;
            if (!std::isfinite(re[(size_t)i]) || !std::isfinite(im[(size_t)i])) finite = false;
        }
        if (!finite) continue;
        fftSplit(re.data(), im.data(), lg, false);
        for (int k = 0; k < N; k++) P[(size_t)((k + N / 2) % N)] += (double)re[(size_t)k] * re[(size_t)k] + (double)im[(size_t)k] * im[(size_t)k];
    }
    std::vector<double> cum((size_t)N + 1, 0.0);
    for (int i = 0; i < N; i++) cum[(size_t)i + 1] = cum[(size_t)i] + P[(size_t)i];
    const double bin = rate_ / N;
    // the two edges of the occupied band (4.5 MHz for the bootstrap, 5.83 MHz for a frame in a 6 MHz channel, up to about 7.8 MHz in an 8 MHz one, depending on the bootstrap's
    // sample rate coefficient and the carriers), in decibels so that the ripple of an echo does not move them: the left edge where the
    // power rises most, the right one where it falls most
    const int S = std::max(2, (int)std::lround(0.06e6 / bin)), W0 = (int)std::lround(4.4e6 / bin), W1 = (int)std::lround(8.0e6 / bin);
    if (W0 + 2 * S + 2 >= N) return 0.0;
    const double floorP = std::max(1e-3 * cum[(size_t)N] / N, 1e-30);   // no edge counts for more than 30 dB (an empty, noise-free stopband)
    auto dB = [&](int a, int b) { return std::log(std::max((cum[(size_t)b] - cum[(size_t)a]) / (b - a), floorP)); };
    std::vector<double> rise((size_t)N, -1e30), fall((size_t)N, -1e30);
    for (int i = S; i + S <= N; i++) { rise[(size_t)i] = dB(i, i + S) - dB(i - S, i); fall[(size_t)i] = -rise[(size_t)i]; }
    double best = -1e30;
    int bi = -1;
    for (int a = S; a + W0 + S <= N; a++)
        for (int b = a + W0; b <= std::min(N - S, a + W1); b++) {
            const double score = rise[(size_t)a] + fall[(size_t)b];
            if (score > best) { best = score; bi = a + b; }   // twice the centre, in bins
        }
    if (bi < 0 || best < std::log(10.0)) return 0.0;   // edges of less than 5 dB each: no channel shape, leave the search at the centre
    return (0.5 * bi - N / 2) * bin;
}

bool Atsc3Sync::cutFrame() {
    DECT2_STAGE("a3.cut");
    struct Timer { double& d; std::chrono::steady_clock::time_point t; ~Timer() { d += std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count(); } } tm{st_.secCut, std::chrono::steady_clock::now()};
    const double frameRate = postBootstrapRate(bs_);
    const double rate = rateEff();
    PreambleParams pp;
    if (!preambleParams(bs_.preambleStructure, pp)) { locked_ = false; searchFrom_ = bsStart_ + (long)(rate * 0.01); return true; }
    const long availEnd = rawStart_ + (long)raw_.size();
    // 1. the Preamble tells the length of the frame (it can be longer than the signalled minimum time to the next frame)
    if (frameSamples_ == 0) {
        const long headSamples = (long)std::ceil((double)(pp.fftSize + pp.guard) * 6 * rate / frameRate) + 64;   // up to six Preamble symbols
        if (availEnd < frameStart_ + headSamples) return false;
        std::vector<cf32> y;   // carrier offset removed at the radio's rate, in the resampler's input pass (no copy of the samples)
        resampleExactRot(raw_.data() + (frameStart_ - rawStart_), (size_t)headSamples, rate, frameRate, y, cfo_, (double)frameStart_);
        if (y.size() > (size_t)(pp.fftSize + pp.guard)) {
            double res = estimateCfoGuard(y.data(), y.size(), pp.fftSize, pp.guard, 1, frameRate);
            if (std::fabs(res) < 0.4 * frameRate / pp.fftSize) { cfo_ += res; st_.cfoHz = cfo_; }
        }
        // read L1 from the head, with a copy that is not modified
        PreambleResult pre = decodePreamble(y.data(), y.size(), bs_);
        if (pre.basicOk) {
            size_t len = pre.detailOk ? frameLengthSamples(bs_, pre.basic, pre.detail) : 0;
            if (len == 0) len = (size_t)(minToNextSec_ * frameRate);   // L1-Detail not readable: take the signalled minimum
            frameExact_ = (double)len * rate / frameRate;
            frameSamples_ = (long)std::ceil(frameExact_);
        } else {
            // not a frame we can read: look for the next bootstrap
            missed_++;
            searchFrom_ = bsStart_ + (long)std::llround((minToNextSec_ - 0.002) * rate);
            locked_ = false;
            return true;
        }
    }
    if (availEnd < frameStart_ + frameSamples_ + 16) return false;
    // 2. the whole frame: carrier offset removed at the radio's rate, then the rate converted to the frame's
    std::vector<cf32> y;
    resampleExactRot(raw_.data() + (frameStart_ - rawStart_), (size_t)(frameSamples_ + 16), rate, frameRate, y, cfo_, (double)frameStart_);
    if (pool_) {
        pool_->submit(std::move(y), bs_);
        st_.frames++;
    } else {
        const auto td = std::chrono::steady_clock::now();
        bool ok = rx_->pushFrame(y.data(), y.size(), bs_);
        st_.secDecode += std::chrono::duration<double>(std::chrono::steady_clock::now() - td).count();
        if (ok) { st_.frames++; missed_ = 0; } else missed_++;
    }
    // the next bootstrap comes when this frame is over. The signalled minimum time to the next frame is not a lower bound to trust: a327hbr.cfile
    // signals 250 ms but sends a frame every 242.7 ms, and searching from 250 ms skipped every second frame (and with it the FEC block that
    // straddles the two)
    long next = frameStart_ + frameSamples_ - (long)(rate * 0.0005);
    searchFrom_ = next;
    predNext_ = bsExact_ + bs_.numSymbols * 3072 / kBootstrapRate * rate + frameExact_;   // frames follow each other without a gap: the next bootstrap starts here
    locked_ = false;
    frameSamples_ = 0;
    return true;
}

void Atsc3Sync::work() {
    for (int guard = 0; guard < 64; guard++) {
        bool progress;
        if (!locked_) progress = search();
        else progress = cutFrame();
        // drop what is no longer needed
        long keepFrom = locked_ ? bsStart_ : std::max(searchFrom_, rawStart_);
        keepFrom -= (long)(rate_ * 0.002);
        if (keepFrom > rawStart_ && keepFrom - rawStart_ < (long)raw_.size()) {
            raw_.erase(raw_.begin(), raw_.begin() + (keepFrom - rawStart_));
            rawStart_ = keepFrom;
        }
        if (!progress) break;
    }
}

} // namespace atsc3
} // namespace dect2
