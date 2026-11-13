// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The four-chain dechirp loop of the ionospheric sounder against the one-sample-at-a-time recurrence: the sums, and the phasors left for the next call,
// agree to a part in 1e9 over runs of every length class, and splitting a run in two calls gives the same as one call.
#include "dect2/iono/iono_dechirp.h"
#include "dect2/common/stable_random.h"
#include <cstdio>
#include <random>
#include <vector>

using namespace dect2;

int main() {
    int fails = 0;
    std::mt19937 rng(11);
    dect2::normal_distribution<float> nd(0.f, 1.f);
    const double fs = 20e6, r = 100e3;
    const iono::cd dw = std::polar(1.0, -2 * M_PI * r / (fs * fs));
    const double beta = std::arg(dw);
    for (size_t n : {size_t(1), size_t(63), size_t(64), size_t(65), size_t(1000), size_t(4099), size_t(15625)}) {
        std::vector<std::complex<float>> x(n);
        for (auto& v : x) v = {nd(rng), nd(rng)};
        const iono::cd w0 = std::polar(1.0, -2 * M_PI * 3.1e6 / fs);
        iono::cd z1 = std::polar(1.0, 0.7), w1 = w0, a1 = 0;
        iono::dechirpScalar(x.data(), n, z1, w1, dw, a1);
        iono::cd z2 = std::polar(1.0, 0.7), w2 = w0, a2 = 0;
        iono::dechirpRun(x.data(), n, z2, w2, dw, beta, a2);
        iono::cd z3 = std::polar(1.0, 0.7), w3 = w0, a3 = 0;
        iono::dechirpRun(x.data(), n / 3, z3, w3, dw, beta, a3);
        iono::dechirpRun(x.data() + n / 3, n - n / 3, z3, w3, dw, beta, a3);
        const double scale = std::sqrt((double)n) + 1;
        const double d2 = std::abs(a1 - a2) / scale, d3 = std::abs(a1 - a3) / scale;
        const double dz = std::abs(z1 - z2) + std::abs(w1 - w2) + std::abs(z1 - z3) + std::abs(w1 - w3);
        if (d2 > 1e-9 || d3 > 1e-9 || dz > 1e-8) { printf("FAIL: n=%zu sum diff %g / %g, phasor diff %g\n", n, d2, d3, dz); fails++; }
    }
    if (!fails) printf("iono dechirp: ok\n");
    return fails ? 1 : 0;
}
