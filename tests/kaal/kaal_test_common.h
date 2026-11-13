// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared by the KAAL codec tests: the fixed test signal (same as gen_golden_codec.py) and golden helpers.
#pragma once
#include "kaal_golden_codec.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <vector>

inline std::vector<int16_t> kaalTestSignal() {   // 1 s, 16 kHz, two tones, truncated to int16
    std::vector<int16_t> x(16000);
    for (int i = 0; i < 16000; i++)
        x[i] = (int16_t)(6000.0 * std::sin(2.0 * 3.141592653589793 * 440.0 * i / 16000.0) +
                         3000.0 * std::sin(2.0 * 3.141592653589793 * 1250.0 * i / 16000.0));
    return x;
}
inline std::vector<uint8_t> kaalGoldBits(int frame) {   // 320 bits, one per byte, MSB first
    std::vector<uint8_t> b(320);
    const char* h = kGoldBitsHex[frame];
    for (int i = 0; i < 80; i++) {
        const char c = h[i];
        const int v = c <= '9' ? c - '0' : c - 'a' + 10;
        for (int j = 0; j < 4; j++) b[i * 4 + j] = (v >> (3 - j)) & 1;
    }
    return b;
}
inline double kaalSnrDb(const int16_t* ref, const int16_t* x, size_t n) {
    double s = 0, e = 0;
    for (size_t i = 0; i < n; i++) { s += (double)ref[i] * ref[i]; const double d = (double)ref[i] - x[i]; e += d * d; }
    return 10 * std::log10((s + 1e-9) / (e + 1e-9));
}
