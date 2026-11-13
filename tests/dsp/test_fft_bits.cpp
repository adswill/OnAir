// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The faster portable kernels give the same bits as the plain loops they replaced (docs/PERFORMANCE-CORE.md):
//  - fftSplit (portable radix-2: the bit reversal as a tiled gather with the first stage, NEON passes of three stages) against the old
//    loop, copied here verbatim, for every size 2 .. 2^16, forward and inverse;
//  - RationalResampler (NEON: interleaved samples, outputs in order) against the split path's arithmetic (the portable desamp's
//    8 partial sums), for several ratios and random block sizes.
// Built with the portable kernels (DECT2_PORTABLE) from the sources themselves, so it checks what Windows and Linux run on any machine.
#include "dect2/dsp/dsp_compat.h"
#include "dect2/dsp/resampler.h"
#include "dect2/dsp/simd.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <utility>
#include <vector>

using namespace dect2;

namespace {

// the old portable FFT (dsp_compat.cpp before 0.3.0), unchanged, compiled the way it was (an AVX2 + FMA copy on x86, picked at run time)
DECT2_SIMD_BODY void refForwardBody(float* __restrict re, float* __restrict im, int log2n) {
    const int n = 1 << log2n;
    std::vector<uint32_t> rev(n);
    for (int i = 0; i < n; i++) {
        uint32_t r = 0;
        for (int b = 0; b < log2n; b++) if (i & (1 << b)) r |= 1u << (log2n - 1 - b);
        rev[i] = r;
    }
    std::vector<float> wrv(n), wiv(n);
    for (int h = 1; h < n; h <<= 1)
        for (int k = 0; k < h; k++) {
            const double a = -M_PI * k / h;
            wrv[h - 1 + k] = (float)std::cos(a);
            wiv[h - 1 + k] = (float)std::sin(a);
        }
    for (int i = 0; i < n; i++) {
        const uint32_t j = rev[i];
        if (j > (uint32_t)i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int i = 0; i < n; i += 2) {
        const float ar = re[i], ai = im[i], br = re[i + 1], bi = im[i + 1];
        re[i] = ar + br; im[i] = ai + bi; re[i + 1] = ar - br; im[i + 1] = ai - bi;
    }
    for (int h = 2; h < n; h <<= 1) {
        const float* __restrict wr = &wrv[h - 1];
        const float* __restrict wi = &wiv[h - 1];
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

DECT2_AVX2_DISPATCH(void, refForward, (float* __restrict re, float* __restrict im, int log2n), (re, im, log2n))


int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); failures++; }
}

} // namespace

int main() {
    std::mt19937 rng(11);
    std::normal_distribution<float> nd(0.f, 1.f);
    for (int lg = 1; lg <= 16; lg++) {
        const int n = 1 << lg;
        std::vector<float> re(n), im(n);
        for (int i = 0; i < n; i++) { re[i] = nd(rng); im[i] = nd(rng); }
        for (int inv = 0; inv < 2; inv++) {
            std::vector<float> a = re, b = im, c = re, d = im;
            fftSplit(a.data(), b.data(), lg, inv != 0);
            if (inv) refForward(d.data(), c.data(), lg); else refForward(c.data(), d.data(), lg);
            char what[64];
            std::snprintf(what, sizeof what, "fft 2^%d %s", lg, inv ? "inverse" : "forward");
            check(std::memcmp(a.data(), c.data(), sizeof(float) * n) == 0 && std::memcmp(b.data(), d.data(), sizeof(float) * n) == 0, what);
        }
    }

    // resampler: the default path (interleaved where NEON runs) against the split path (deinterleave, desamp per phase), random block sizes
    struct Case { double in, out, occ; };
    for (const Case& cs : {Case{10e6, 64e6 / 7, 0}, Case{10e6, 64e6 / 7, 6817 * (64e6 / 7) / 8192.0}, Case{2.4e6, 48e3, 12500},
                           Case{2.5e6, 2.048e6, 0}, Case{8e6, 6e6, 0}}) {
        std::vector<cf32> in(400000);
        for (auto& v : in) v = cf32(nd(rng), nd(rng));
        RationalResampler a; a.configure(cs.in, cs.out, nullptr, cs.occ);
        std::vector<cf32> outA;
        for (size_t i = 0; i < in.size();) {
            const size_t m = std::min<size_t>(in.size() - i, 1 + rng() % 30000);
            a.process(in.data() + i, m, outA);
            i += m;
        }
        RationalResampler b; b.configure(cs.in, cs.out, nullptr, cs.occ); b.allowInterleaved(false);
        std::vector<cf32> outB;
        for (size_t i = 0; i < in.size();) {
            const size_t m = std::min<size_t>(in.size() - i, 1 + rng() % 20000);
            b.process(in.data() + i, m, outB);
            i += m;
        }
        const size_t nn = std::min(outA.size(), outB.size());
        char what[96];
        std::snprintf(what, sizeof what, "resampler %.4g -> %.4g: interleaved and split paths give the same bits", cs.in, cs.out);
        check(nn > 1000 && std::memcmp(outA.data(), outB.data(), nn * sizeof(cf32)) == 0, what);
    }
    if (failures) return 1;
    std::printf("fft and resampler kernels: same bits as the plain loops\n");
    return 0;
}
