// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Synthetic Loran-C pulse groups for tests: one master and any number of secondaries at a chosen GRI, white noise at a chosen SNR.
// Complex baseband at fs with the 100 kHz carrier at carrierHz (the receiver's signal offset).
// The 9th pulse of the master takes the phase of its 8th pulse (its own code is not in the public text used).
#pragma once
#include "dect2/eloran/eloran_rx.h"
#include <cmath>
#include <complex>
#include <cstdint>
#include <random>
#include <vector>

namespace dect2::loran {

struct GenStation {
    bool master = false;
    double delayUs = 0;     // start of the group after the master's group start
    double amp = 1.0;       // peak carrier amplitude
};

struct GenParams {
    double fs = 250000.0;
    double seconds = 2.0;
    int griUs = 99600;
    double carrierHz = 0.0;
    double firstGroupUs = 12345.0;   // where the master's first group starts
    std::vector<GenStation> stations;
    double snrDb = 1e9;              // peak pulse power over the noise power in 20 kHz; 1e9 = no noise
    uint32_t seed = 1;
};

inline std::vector<std::complex<float>> generateLoran(const GenParams& g) {
    const size_t n = (size_t)(g.seconds * g.fs);
    std::vector<std::complex<double>> y(n, std::complex<double>(0, 0));
    const double twoPi = 6.283185307179586;
    const int periods = (int)(g.seconds * 1e6 / g.griUs) + 2;
    for (int i = 0; i < periods; i++) {
        const char code = (i & 1) ? 'B' : 'A';
        for (const GenStation& s : g.stations) {
            const char* pat = phasePattern(s.master, code);
            const int np = s.master ? 9 : 8;
            for (int k = 0; k < np; k++) {
                const double start = g.firstGroupUs + s.delayUs + (double)i * g.griUs + (k == 8 ? 9000.0 : k * 1000.0);
                const double sign = pat[k == 8 ? 7 : k] == '+' ? 1.0 : -1.0;
                const long i0 = (long)std::ceil(start * 1e-6 * g.fs);
                for (long q = i0; q < (long)n; q++) {
                    const double t = (double)q / g.fs * 1e6 - start;
                    if (t > 600.0) break;
                    const double e = pulseEnvelope(t);
                    if (e <= 0) continue;
                    const double ph = twoPi * g.carrierHz * (double)q / g.fs;
                    y[(size_t)q] += sign * s.amp * e * std::complex<double>(std::cos(ph), std::sin(ph));
                }
            }
        }
    }
    std::vector<std::complex<float>> out(n);
    std::mt19937 rng(g.seed);
    std::normal_distribution<double> nd(0.0, 1.0);
    double sigma = 0;
    if (g.snrDb < 1e8) sigma = std::sqrt(std::pow(10.0, -g.snrDb / 10.0) * g.fs / 20000.0 / 2.0);   // per component
    for (size_t q = 0; q < n; q++) {
        std::complex<double> v = y[q];
        if (sigma > 0) v += std::complex<double>(sigma * nd(rng), sigma * nd(rng));
        out[q] = std::complex<float>((float)v.real(), (float)v.imag());
    }
    return out;
}

}  // namespace dect2::loran
