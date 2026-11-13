// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The two-minimum correction of the min-sum check rule, shared by the LDPC decoders (DVB-S2/S2X, DTMB, ATSC 3.0) and mirrored in the GPU kernels.
// Every edge of a check except the one holding the smallest input sees both of the two smallest inputs; the box-plus of those two,
// min1 + log(1 + e^-(min1 + min2)) - log(1 + e^-(min2 - min1)), is closer to what belief propagation sends than min1 alone (0.1 to 0.3 dB on the
// DVB-S2 codes at the threshold, and fewer iterations). It needs the LLRs in their true scale.
#pragma once
#include <algorithm>

namespace dect2 {

// log(1 + e^-t) for t >= 0 as the largest of four tangents of the curve (it is convex): at most 0.02 below it, branch free (vectorises)
inline float softplusNeg(float t) {
    const float a = std::max(0.6854f - 0.4378f * t, 0.5822f - 0.2689f * t), b = std::max(0.3653f - 0.1192f * t, 0.1323f - 0.0293f * t);
    return std::max(0.f, std::max(a, b));
}

// the corrected smallest magnitude (never above min1)
inline float boxMin1(float min1, float min2) {
    const float c = min1 + softplusNeg(min1 + min2) - softplusNeg(min2 - min1);
    return c > 0.f ? c : 0.f;
}

} // namespace dect2
