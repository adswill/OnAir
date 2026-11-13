// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared helpers of the KAIT tests: a synthetic 1080p test picture (gradients, text-like edges, noise texture, discs), seeded Gaussian noise.
#pragma once
#include "dect2/kait/kait_codec.h"
#include "dect2/kris/kris_common.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>

namespace kaittest {

struct Rng {
    dect2::kris::XorShift64 x;
    bool have = false;
    double spare = 0.0;
    explicit Rng(uint64_t seed) : x{seed * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull} {}
    double uniform() { return (double)(x.next() >> 11) * (1.0 / 9007199254740992.0); }
    uint64_t below(uint64_t n) { return x.next() % n; }
    double gauss() {
        if (have) { have = false; return spare; }
        double u1 = uniform();
        const double u2 = uniform();
        if (u1 < 1e-300) u1 = 1e-300;
        const double r = std::sqrt(-2.0 * std::log(u1));
        const double a = 6.283185307179586 * u2;
        spare = r * std::sin(a);
        have = true;
        return r * std::cos(a);
    }
};

inline dect2::kait::Image testPicture(int w = 1920, int h = 1080) { return dect2::kait::syntheticPicture(w, h); }

}  // namespace kaittest

static int fails = 0;
#define CHECK(c, fmt, ...) do { if (!(c)) { printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)   // Flawfinder: ignore (the format is a literal)

#include "dect2/kait/kait_frame.h"
#include "dect2/kait/kait_rx.h"
namespace kaittest {
using dect2::kait::cf;

// SNR is the signal power over the noise power in the 190 kHz occupied band (the burst has unit average power at 632 ksps). The channel: delay of `lead`
// samples, a sample clock error of `clockPpm` (the receiver's clock is fast by that much), a carrier offset, noise.
inline std::vector<cf> channel(const std::vector<cf>& burst, double snrDb, double cfoHz, double clockPpm, size_t lead, size_t tail, Rng& r) {
    const double fs = dect2::kait::kInternalRate;
    std::vector<cf> src = burst;
    if (clockPpm != 0.0) {   // y[n] = x(n / (1 + e)): the receiver clock is fast by e (it takes 4 (1 + e) samples per symbol)
        const double e = clockPpm * 1e-6;
        const size_t n = (size_t)((double)burst.size() * (1.0 + std::fabs(e))) - 8;
        src.assign(n, cf{0, 0});
        for (size_t i = 0; i < n; i++) {
            const double pos = (double)i / (1.0 + e);
            const long k = (long)std::floor(pos);
            const double f = pos - (double)k;
            if (k < 1 || k + 2 >= (long)burst.size()) continue;
            const float fl = (float)f;
            const float w0 = -fl * (fl - 1) * (fl - 2) / 6.f, w1 = (fl + 1) * (fl - 1) * (fl - 2) / 2.f, w2 = -(fl + 1) * fl * (fl - 2) / 2.f, w3 = (fl + 1) * fl * (fl - 1) / 6.f;
            src[i] = burst[(size_t)k - 1] * w0 + burst[(size_t)k] * w1 + burst[(size_t)k + 1] * w2 + burst[(size_t)k + 2] * w3;
        }
    }
    std::vector<cf> out(lead + src.size() + tail);
    const double sg = snrDb > 200.0 ? 0.0 : std::sqrt(dect2::kait::kInternalRate / dect2::kait::kBandwidthHz / std::pow(10.0, snrDb / 10.0) / 2.0);
    const double ph0 = 6.283185307179586 * r.uniform();
    for (size_t i = 0; i < out.size(); i++) {
        cf v{0.f, 0.f};
        if (sg > 0.0) v = cf{(float)(sg * r.gauss()), (float)(sg * r.gauss())};
        if (i >= lead && i < lead + src.size()) {
            const double ph = ph0 + 6.283185307179586 * cfoHz * (double)i / fs;
            v += src[i - lead] * cf{(float)std::cos(ph), (float)std::sin(ph)};
        }
        out[i] = v;
    }
    return out;
}
}  // namespace kaittest
