// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Fast phase of a complex sample for the detectors that only need a few milliradians: atan2 from a 4 coefficient polynomial on min/max
// (maximum error about 1e-4 rad = 0.006 degrees, no libm call, a division and a few multiplications; libm's atan2f is 10 times slower).
#pragma once
#include <cmath>

namespace dect2 {

inline float fastAtan2(float y, float x) {
    const float ax = std::fabs(x), ay = std::fabs(y);
    const float mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax;
    const float a = mx > 0.f ? mn / mx : 0.f;
    const float s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = 1.57079632679f - r;
    if (x < 0.f) r = 3.14159265359f - r;
    return y < 0.f ? -r : r;
}

} // namespace dect2
