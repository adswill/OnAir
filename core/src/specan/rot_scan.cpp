// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/specan/rot_scan.h"
#include <cmath>

namespace dect2 {

ScanPeakResult scanPeakParabolic(const std::vector<double>& levelDb, double stepDeg, double& peakOffsetDeg) {
    const int n = (int)levelDb.size();
    if (n < 3 || !(stepDeg > 0)) return ScanPeakResult::TooFew;
    int best = 0;
    for (int i = 1; i < n; i++) if (levelDb[(size_t)i] > levelDb[(size_t)best]) best = i;
    double lo = levelDb[0], hi = levelDb[0];
    for (double v : levelDb) { if (v < lo) lo = v; if (v > hi) hi = v; }
    if (hi - lo < 1e-9) return ScanPeakResult::Flat;
    if (best == 0 || best == n - 1) { peakOffsetDeg = best * stepDeg; return ScanPeakResult::AtEdge; }
    const double y0 = levelDb[(size_t)best - 1], y1 = levelDb[(size_t)best], y2 = levelDb[(size_t)best + 1];
    const double den = y0 - 2.0 * y1 + y2;
    double delta = 0.0;
    if (den < 0.0) delta = 0.5 * (y0 - y2) / den;   // vertex of the parabola through the three points, in samples from best
    if (delta > 0.5) delta = 0.5;
    if (delta < -0.5) delta = -0.5;
    peakOffsetDeg = (best + delta) * stepDeg;
    return ScanPeakResult::Ok;
}

double scanPeakMechAz(double scanStartMechDeg, double peakOffsetDeg) {
    double a = std::fmod(scanStartMechDeg + peakOffsetDeg, 360.0);
    if (a < 0) a += 360.0;
    return a;
}

} // namespace dect2
