// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mathematical and physical constants shared by every mode (previously redefined as a local kPi etc. in ~160 files).
// Only exact, full-precision values live here; modes that deliberately use a different pi (e.g. IS-GPS-200's
// 3.1415926535898) keep their own constant.
#pragma once

namespace dect2 {
namespace math {

// pi to double precision (same value as M_PI).
inline constexpr double kPi = 3.14159265358979323846;
// 1/sqrt(2) as the float literal used by the QPSK/constellation tables (0.70710678f).
inline constexpr float k1OverSqrt2f = 0.70710678f;
// Speed of light in vacuum, m/s (exact, SI definition).
inline constexpr double kSpeedOfLight = 299792458.0;

} // namespace math
} // namespace dect2
