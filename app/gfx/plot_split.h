// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Where the boundary between the spectrum and the waterfall goes in a panel h pixels high (no ImGui: tested in test_general_layout).
// The waterfall always gets its share of the real panel: a panel too short for both minimum sizes used to put the waterfall below
// its bottom edge (clipped away, so it looked gone until the panel grew), and a NaN or out-of-range fraction is replaced.
#pragma once
#include <algorithm>
#include <cmath>

namespace plotsplit {

constexpr float kFracMin = 0.2f, kFracMax = 0.8f, kFracDefault = 0.5f;
constexpr float kMinSpec = 60.f, kMinWf = 60.f;   // the least each plot is given when the panel has room (the spectrum's counts its band strip)

inline float safeFrac(float f) { return std::isfinite(f) ? std::min(kFracMax, std::max(kFracMin, f)) : kFracDefault; }

// The height of the spectrum part (strip included); the waterfall has h - result. Both stay inside h.
inline float spectrumHeight(float h, float frac, float strip) {
    if (!(h >= 2.f)) return 1.f;
    strip = std::isfinite(strip) && strip > 0 ? strip : 0.f;
    const float minS = kMinSpec + strip;
    if (h < minS + kMinWf) return std::max(1.f, std::floor(h * minS / (minS + kMinWf)));   // too short for both minimums: share it
    return std::floor(std::min(std::max(minS, h * safeFrac(frac)), h - kMinWf));
}

}  // namespace plotsplit
