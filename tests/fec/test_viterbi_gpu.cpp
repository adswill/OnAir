// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The GPU Viterbi (GpuViterbi, Metal) against the exact CPU decoder (dvbt::Viterbi::decode, 16-bit): the same bits and the same summed path
// metric on DVB-T-sized blocks (noisy, punctured and full-scale soft values), and the time each takes. Passes without a GPU (nothing to compare).
#include "dect2/dsp/gpu_viterbi.h"
#include "dect2/dvbt/dvbt.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace dect2;

int main() {
    if (!GpuViterbi::instance().available()) { printf("viterbi gpu: no GPU, nothing to compare\n"); return 0; }
    std::mt19937 rng(23);
    std::normal_distribution<double> nd(0, 1);
    int fails = 0;
    for (int round = 0; round < 6; round++) {
        const size_t steps = round < 3 ? 387072 : 100000 + 9973 * round;   // a 16-symbol 64-QAM 2/3 block, and odd sizes
        const double amp = round == 2 ? 127 : 22, sig = round == 1 ? 1.1 : 0.6;
        std::vector<int8_t> soft(2 * steps);
        unsigned reg = 0;
        for (size_t t = 0; t < steps; t++) {
            reg = ((unsigned)(rng() & 1) << 6) | (reg >> 1);
            const int x = __builtin_parity(reg & 0x79), y = __builtin_parity(reg & 0x5B);
            for (int k = 0; k < 2; k++) {
                double v = ((k ? y : x) ? -amp : amp) + amp * sig * nd(rng);
                if ((t % 2) && k) v = 0;   // 2/3 puncturing
                soft[2 * t + k] = (int8_t)std::lround(std::max(-127.0, std::min(127.0, v)));
            }
        }
        std::vector<uint8_t> a, b;
        long ma = 0, mb = 0;
        const auto t0 = std::chrono::steady_clock::now();
        dvbt::Viterbi::decode(soft, a, 4, &ma, false);
        const auto t1 = std::chrono::steady_clock::now();
        const bool ok = GpuViterbi::instance().decode(soft, b, &mb);
        const auto t2 = std::chrono::steady_clock::now();
        const bool same = ok && a == b && ma == mb;
        printf("%zu steps: %s (CPU %.2f ms on 4 threads, GPU %.2f ms)\n", steps, same ? "identical" : "DIFFERENT", std::chrono::duration<double, std::milli>(t1 - t0).count(),
               std::chrono::duration<double, std::milli>(t2 - t1).count());
        if (!same) fails++;
    }
    printf(fails ? "viterbi gpu: %d FAILED\n" : "viterbi gpu: ok\n", fails);
    return fails ? 1 : 0;
}
