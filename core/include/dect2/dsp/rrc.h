// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Root raised cosine pulse shared by the test signal generators and the matched filters of the satellite and HF data modes (GOES, Meteor, HRPT, Elektro, HFDL).
#pragma once
#include <cmath>
#include "dect2/common/mathconst.h"

namespace dect2 {
namespace dsp {

// The impulse response at time t (in symbols) of the root raised cosine filter with roll-off a, unit energy per symbol, including the two removable singularities
// (t = 0 and |t| = 1 / (4 a)).
inline double rrc(double t, double a) {
    const double kPi = dect2::math::kPi;
    if (std::fabs(t) < 1e-9) return 1 - a + 4 * a / kPi;
    if (a > 0 && std::fabs(std::fabs(t) - 1 / (4 * a)) < 1e-9)
        return a / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * a)) + (1 - 2 / kPi) * std::cos(kPi / (4 * a)));
    return (std::sin(kPi * t * (1 - a)) + 4 * a * t * std::cos(kPi * t * (1 + a))) / (kPi * t * (1 - 16 * a * a * t * t));
}

} // namespace dsp
} // namespace dect2
