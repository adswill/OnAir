// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Spectrum engine alone: CPU seconds per signal second for the sample rates and FFT sizes the analyser runs at.
// Feeds synthetic IQ (a few tones plus noise) in 64k-sample pieces like the engine does and takes a frame 30 times per signal second,
// like the display does. Usage: bench_spectrum [seconds_of_signal=2]. Prints one line per rate and size; no pass/fail.
#include "dect2/dsp/spectrum.h"
#include "dect2/common/stable_random.h"
#include <ctime>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
using namespace dect2;

int main(int argc, char** argv) {
    const double secs = argc > 1 ? atof(argv[1]) : 2.0;
    const double rates[] = {2e6, 10e6, 20e6};
    const size_t sizes[] = {1024, 4096, 16384, 65536};
    std::mt19937 rng(1);
    dect2::normal_distribution<float> nd(0.f, 0.02f);
    const size_t N = 1 << 20;   // one second of the slowest rate, replayed
    std::vector<cf32> sig(N);
    for (size_t i = 0; i < N; i++) {
        const float t = (float)i;
        const float a = 0.3f * std::cos(0.05f * t) + 0.15f * std::cos(0.7f * t);
        const float b = 0.3f * std::sin(0.05f * t) + 0.15f * std::sin(0.7f * t);
        sig[i] = cf32{a + nd(rng), b + nd(rng)};
    }
    printf("%-8s %-8s %-14s\n", "Msps", "fft", "cpu_s/signal_s");
    double chk = 0;
    for (double fs : rates)
        for (size_t fft : sizes) {
            SpectrumAnalyzer an(fft); if (getenv("STATS_ONLY")) an.setTransform(false);   // dev: the statistics pass alone
            SpectrumFrame fr;
            const size_t total = (size_t)(secs * fs), piece = 65536;
            const size_t perFrame = (size_t)(fs / 30);
            size_t done = 0, sinceFrame = 0, pos = 0;
            const std::clock_t t0 = std::clock();   // CPU time, so a busy machine does not skew it
            while (done < total) {
                const size_t n = std::min(piece, total - done);
                if (pos + n > N) pos = 0;
                an.feed(sig.data() + pos, n);
                pos += n; done += n; sinceFrame += n;
                if (sinceFrame >= perFrame) { sinceFrame = 0; if (an.takeFrame(fr)) chk += fr.dbfs[fft / 3]; }
            }
            const double cpu = (double)(std::clock() - t0) / CLOCKS_PER_SEC;
            printf("%-8.0f %-8zu %.4f\n", fs / 1e6, fft, cpu / secs);
        }
    fprintf(stderr, "checksum %.3f\n", chk);
    return 0;
}
