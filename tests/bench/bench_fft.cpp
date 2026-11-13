// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The complex FFT alone (fftSplit) at the sizes the spectrum uses: microseconds per transform. Build with -DDECT2_PORTABLE to time the
// portable kernels (Windows, Linux) on any machine; without it Apple's vDSP is timed. Usage: bench_fft [seconds_per_size=0.4]
#include "dect2/dsp/dsp_compat.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <vector>
int main(int argc, char** argv) {
    const double secs = argc > 1 ? atof(argv[1]) : 0.4;
    printf("%-8s %12s %10s\n", "size", "best us", "Mpts/s");
    for (int lg = 10; lg <= 20; lg++) {
        const int n = 1 << lg;
        std::vector<float> re(n), im(n);
        for (int i = 0; i < n; i++) { re[i] = std::sin(0.01f * i); im[i] = std::cos(0.013f * i); }
        dect2::fftSplit(re.data(), im.data(), lg, false);   // plan, warm caches
        // the fastest of many transforms: a loaded machine adds time, never takes it away
        double best = 1e30;
        const auto t0 = std::chrono::steady_clock::now();
        double el = 0;
        do {
            const auto a = std::chrono::steady_clock::now();
            dect2::fftSplit(re.data(), im.data(), lg, false);
            const double d = std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
            best = std::min(best, d);
            for (int i = 0; i < n; i += 64) { re[i] *= 0.5f; im[i] *= 0.5f; }   // (keeps the values finite)
            el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        } while (el < secs);
        const double us = 1e6 * best;
        printf("%-8d %12.1f %10.1f\n", n, us, n / us);
    }
    return 0;
}
