// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pure helpers of the spectrum plots (no ImGui): the trace decimation and the frequency tick labels. Tested in tests/app/test_plot_util.cpp.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace plt {

// The trace at the width of the plot: only the visible bins, and where they are denser than the pixels (more than two a column), the lowest and
// the highest of each pixel column (an envelope), so no narrow peak is lost. The bin i sits at x0 + i * dx. ox/oy are cleared and refilled.
inline void envelope(const std::vector<float>& y, double x0, double dx, double lo, double hi, int px, std::vector<double>& ox, std::vector<double>& oy) {
    ox.clear(); oy.clear();
    const size_t n = y.size();
    if (n < 2 || !(hi > lo) || !(dx > 0)) return;
    size_t i0 = (size_t)std::max(0.0, std::floor((lo - x0) / dx) - 1), i1 = (size_t)std::min((double)n - 1, std::ceil((hi - x0) / dx) + 1);
    if (i0 >= n) i0 = n - 1;
    if (i1 <= i0) i1 = std::min(n - 1, i0 + 1);
    const size_t cnt = i1 - i0 + 1;
    if (px < 1 || cnt <= (size_t)px * 2) { for (size_t i = i0; i <= i1; i++) { ox.push_back(x0 + i * dx); oy.push_back(y[i]); } return; }
    for (int c = 0; c < px; c++) {
        const size_t a0 = i0 + cnt * c / px, a1 = std::max(a0 + 1, i0 + cnt * (c + 1) / px);
        size_t imn = a0, imx = a0;
        for (size_t i = a0; i < a1 && i < n; i++) { if (y[i] < y[imn]) imn = i; if (y[i] > y[imx]) imx = i; }
        const size_t f = std::min(imn, imx), g = std::max(imn, imx);
        ox.push_back(x0 + f * dx); oy.push_back(y[f]);
        if (g != f) { ox.push_back(x0 + g * dx); oy.push_back(y[g]); }
    }
}

// A frequency tick label in MHz with just enough decimals for the tick step (0.5 MHz: "100.5"; 5 kHz: "100.005"; 2 MHz: "100").
inline std::string formatFreqTick(double mhz, double stepMhz) {
    int dec = 0;
    if (stepMhz > 0 && std::isfinite(stepMhz)) dec = std::max(0, std::min(6, (int)std::ceil(-std::log10(stepMhz) - 1e-9)));
    char b[40];
    snprintf(b, sizeof b, "%.*f", dec, std::fabs(mhz) < stepMhz * 1e-6 ? 0.0 : mhz);
    return b;
}

// A frequency with its unit for the readout: "1.2 GHz" / "100.500 MHz" / "12.5 kHz" / "800 Hz".
inline std::string formatFreqUnit(double mhz) {
    char b[40];
    const double a = std::fabs(mhz);
    if (a >= 1000) snprintf(b, sizeof b, "%.4f GHz", mhz / 1000);
    else if (a >= 1) snprintf(b, sizeof b, "%.4f MHz", mhz);
    else if (a >= 0.001) snprintf(b, sizeof b, "%.2f kHz", mhz * 1000);
    else snprintf(b, sizeof b, "%.0f Hz", mhz * 1e6);
    return b;
}

// Alpha (0..1) at the bottom of a faded fill, from the fade amount (0: none).
// Wheel or pinch zoom: the factor that keeps the new span inside minSpan..maxSpan. At a limit it is 1, so the view stays where it is (#425, #442).
inline double zoomFactorWithin(double span, double factor, double minSpan, double maxSpan) {
    if (!(span > 0) || !std::isfinite(span) || !std::isfinite(factor)) return 1;
    return std::max(minSpan, std::min(maxSpan, span * factor)) / span;
}
// The narrowest span a plot can still draw at these values: below it the tick steps vanish in the rounding of a double, and a tick loop never ends (#442).
inline double numericMinSpan(double lo, double hi) { return std::max(1e-12, std::max(std::fabs(lo), std::fabs(hi)) * 1e-11); }
// A zoom left the dB view [lo, hi] above or below the whole trace [traceLo, traceHi] (#425): move the view, same span, so the nearest part of the trace
// shows again, inside absLo..absHi. false: some of the trace is in view already, nothing changes.
inline bool keepTraceInView(float& lo, float& hi, float traceLo, float traceHi, float absLo, float absHi) {
    if (!(traceHi >= traceLo) || !(hi > lo) || (traceHi >= lo && traceLo <= hi)) return false;
    const float span = hi - lo;
    if (traceHi < lo) { lo = traceHi - span * 0.8f; hi = lo + span; }
    else { hi = traceLo + span * 0.8f; lo = hi - span; }
    if (lo < absLo) { lo = absLo; hi = lo + span; }
    if (hi > absHi) { hi = absHi; lo = hi - span; }
    return true;
}
inline float fillBottomAlpha(float fade) { return 1.f - std::min(1.f, std::max(0.f, fade)); }

// Alpha factor of a dense scatter of n dots (alpha accumulation): 1 up to 600 dots, then falling with the square root, never below 0.22.
inline float scatterAlphaFactor(int n) { return n <= 600 ? 1.f : std::max(0.22f, std::sqrt(600.f / (float)n)); }

} // namespace plt
