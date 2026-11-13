// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A complex-signal FIR dot product the compiler can vectorise without -ffast-math: real taps written twice (h0 h0 h1 h1 ...) against the
// interleaved re / im floats of the samples, summed into sixteen independent accumulators (four 4-lane vectors), so that the multiply-adds
// pipeline instead of waiting on one running sum. Used by the transmit generators whose channel / resampling filters were their hot spot.
#pragma once
#include "dect2/common/ring.h"
#include <vector>

namespace dect2 {

// taps h[0..L) -> the doubled layout firDot expects (2L floats)
inline std::vector<float> firDoubleTaps(const float* h, int L) {
    std::vector<float> d((size_t)(2 * L));
    for (int k = 0; k < L; k++) d[(size_t)(2 * k)] = d[(size_t)(2 * k + 1)] = h[k];
    return d;
}

// sum over k < L of h[k] * x[k], with h2 from firDoubleTaps
inline cf32 firDot(const float* h2, const cf32* x, int L) {
    const float* xf = reinterpret_cast<const float*>(x);
    const int n = 2 * L;
    float a[16] = {};
    int k = 0;
    for (; k + 16 <= n; k += 16)
        for (int j = 0; j < 16; j++) a[j] += h2[k + j] * xf[k + j];
    for (; k + 4 <= n; k += 4)
        for (int j = 0; j < 4; j++) a[j] += h2[k + j] * xf[k + j];
    for (; k < n; k += 2) { a[0] += h2[k] * xf[k]; a[1] += h2[k + 1] * xf[k + 1]; }
    float re = 0.f, im = 0.f;
    for (int j = 0; j < 16; j += 2) { re += a[j]; im += a[j + 1]; }
    return cf32(re, im);
}

} // namespace dect2
