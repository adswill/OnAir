// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The analyser's fast dB conversion against std::log10 over the whole range it is used in: it must stay within 0.0001 dB.
#include "dect2/dsp/spectrum.h"
#include "dect2/common/stable_random.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;

int main() {
    std::mt19937 rng(7);
    dect2::uniform_real_distribution<double> ex(-14.0, 4.0);
    double worst = 0;
    for (int i = 0; i < 2000000; i++) {
        const float p = (float)std::pow(10.0, ex(rng));
        worst = std::max(worst, std::fabs((double)spectrumDb(p) - 10.0 * std::log10((double)p)));
    }
    for (float p : {1e-14f, 1.f, 2.f, 0.5f, 1.41421356f, 1.4142135f, 1.4142137f, 1024.f}) worst = std::max(worst, std::fabs((double)spectrumDb(p) - 10.0 * std::log10((double)p)));
    printf("worst error %.6f dB\n", worst);
    if (worst > 1e-4) { printf("FAIL\n"); return 1; }
    printf("OK\n");
    return 0;
}
