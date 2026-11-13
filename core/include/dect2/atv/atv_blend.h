// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Analog TV temporal noise reduction: one picture blended into the previous one, per colour byte (alpha untouched).
// The same arithmetic as the per-byte reference (double weight from |c - p|, lround), with the weight of each of the 256 possible
// differences worked out once per call instead of once per byte; tests/atv/test_atv_blend.cpp compares it with the reference on every input.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace dect2 {

inline void atvTemporalBlend(uint8_t* cur, const uint8_t* prev, size_t n, double k, double T) {
    double wt[256];
    for (int d = 0; d < 256; d++) wt[d] = d <= T ? k : std::min(1.0, k + (d - T) / T);
    for (size_t i = 0; i < n; i += 4) {
        for (size_t j = 0; j < 3; j++) {
            const int c = cur[i + j], p = prev[i + j];
            const double x = p + (c - p) * wt[c > p ? c - p : p - c];   // between p and c, so never negative: lround is a round-half-up
            const int fl = (int)x;
            cur[i + j] = (uint8_t)(fl + (x - fl >= 0.5 ? 1 : 0));
        }
    }
}

} // namespace dect2
