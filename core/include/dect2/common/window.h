// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Window-design helpers shared by the filter builders: the modified Bessel function I0 used by Kaiser
// windows and the normalised sinc. Different modes truncated the I0 power series at different term counts
// (and some stop early at a relative tolerance); those choices change the last digits, so they stay explicit
// parameters instead of being unified: callers pass exactly the values their original private copy used.
#pragma once
#include <cmath>

namespace dect2 {
namespace window {

// Modified Bessel function of the first kind, order 0, by its power series sum((x/2)^2k / (k!)^2):
// terms k = 1 .. maxTerms-1 are added; with relTol > 0 the loop also stops once a term drops below relTol * sum.
inline double besselI0(double x, int maxTerms = 40, double relTol = 0.0) {
    double s = 1, t = 1;
    for (int k = 1; k < maxTerms; k++) {
        t *= (x / (2.0 * k)) * (x / (2.0 * k));
        s += t;
        if (t < relTol * s) break;
    }
    return s;
}

} // namespace window
} // namespace dect2
