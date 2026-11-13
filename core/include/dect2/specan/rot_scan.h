// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Peak of an azimuth scan (rotator north-offset calibration): the level at each step, with a parabolic fit around the maximum.
#pragma once
#include <vector>

namespace dect2 {

enum class ScanPeakResult { Ok, TooFew, Flat, AtEdge };

// levelDb[i] was measured at scan offset i * stepDeg from the scan start. On Ok, peakOffsetDeg is the interpolated offset of the top
// (same reference as the scan start). AtEdge: the maximum is the first or last sample, so the satellite lies outside the scan.
ScanPeakResult scanPeakParabolic(const std::vector<double>& levelDb, double stepDeg, double& peakOffsetDeg);

// Mechanical azimuth at scan offset peakOffsetDeg, from the scan start in mechanical degrees (wraps to 0..360).
double scanPeakMechAz(double scanStartMechDeg, double peakOffsetDeg);

} // namespace dect2
