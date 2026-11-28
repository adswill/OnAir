// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pure helpers of the Microtelecom Perseus driver (source_perseus.cpp): the sample format and the attenuator steps. No library, no hardware.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace dect2 {
namespace native {

// The Perseus delivers 24-bit signed samples, little-endian: 3 bytes I then 3 bytes Q for each complex sample. Output: float I/Q pairs
// scaled so that full scale of the 24-bit value is 1.0. Returns the number of complex samples written (a partial sample at the end is dropped).
inline size_t perseusConvert(const uint8_t* in, size_t bytes, float* out) {
    constexpr float kInv = 1.0f / 8388608.0f;   // 2^23
    const size_t n = bytes / 6;
    for (size_t i = 0; i < n; i++) {
        for (int c = 0; c < 2; c++) {
            const uint8_t* p = in + i * 6 + (size_t)c * 3;
            int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
            if (v & 0x800000) v -= 0x1000000;   // sign extension from 24 bits
            out[2 * i + (size_t)c] = (float)v * kInv;
        }
    }
    return n;
}

// The gain slider (0..30 dB, the top is no attenuation) -> the attenuator step: 0 = 0 dB, 1 = -10 dB, 2 = -20 dB, 3 = -30 dB
inline int perseusAttStep(double gainDb) {
    const double g = std::min(std::max(gainDb, 0.0), 30.0);
    return (int)std::lround((30.0 - g) / 10.0);
}

} // namespace native
} // namespace dect2
