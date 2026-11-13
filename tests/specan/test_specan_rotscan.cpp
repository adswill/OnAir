// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Azimuth scan peak (parabolic fit) for the rotator north-offset calibration.
#include "dect2/specan/rot_scan.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    // Exact parabola through -10..10 in 1 degree steps, vertex at 3.4: the fit is exact.
    std::vector<double> p;
    for (int i = 0; i <= 20; i++) { const double x = -10.0 + i; p.push_back(-(x - 3.4) * (x - 3.4)); }
    double off = 0;
    ScanPeakResult r = scanPeakParabolic(p, 1.0, off);
    // scan offsets are i * step, so the scan starts at -10 degrees from the calibration reference: indices run 0..20
    CHECK(r == ScanPeakResult::Ok, "parabola: result %d", (int)r);
    CHECK(std::fabs(off - 13.4) < 1e-9, "parabola vertex %.6f, expected 13.4 (index 13.4 at 1 degree steps)", off);

    // Lorentzian-shaped beam (not a parabola in dB) peaked at 2.6 degrees from a scan starting at -10: small fit bias allowed.
    std::vector<double> g;
    for (int i = 0; i <= 20; i++) { const double x = (-10.0 + i) - 2.6; g.push_back(10.0 * std::log10(1.0 / (1.0 + (x / 4.0) * (x / 4.0)))); }
    r = scanPeakParabolic(g, 1.0, off);
    CHECK(r == ScanPeakResult::Ok, "lorentz: result %d", (int)r);
    CHECK(std::fabs(off - 12.6) < 0.1, "lorentz vertex %.4f, expected 12.6", off);

    // Peak at the first sample: the satellite is outside the scan.
    std::vector<double> e{-1.0, -2.0, -3.0, -4.0, -5.0};
    r = scanPeakParabolic(e, 1.0, off);
    CHECK(r == ScanPeakResult::AtEdge, "edge result %d", (int)r);

    // Flat scan and too few samples.
    std::vector<double> f(5, -40.0);
    CHECK(scanPeakParabolic(f, 1.0, off) == ScanPeakResult::Flat, "flat scan");
    std::vector<double> s{-1.0, -2.0};
    CHECK(scanPeakParabolic(s, 1.0, off) == ScanPeakResult::TooFew, "two samples");

    // Mechanical azimuth wraps: scan from 355 with peak at +10 gives 5.
    CHECK(std::fabs(scanPeakMechAz(355.0, 10.0) - 5.0) < 1e-9, "wrap up");
    CHECK(std::fabs(scanPeakMechAz(5.0, -10.0) - 355.0) < 1e-9, "wrap down");

    return testSummary("specan_rotscan");
}
