// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Speed limits in the tests (CPU seconds per second of signal, load in %, milliseconds per call) are tuned on a developer machine. The CI runners
// are shared and slower, so there the limits are twice as wide: a real slowdown (a factor of two or more) still fails.
#pragma once
#include <cstdlib>

inline double ciSlack() {
    static const double v = std::getenv("GITHUB_ACTIONS") ? 2.0 : 1.0;
    return v;
}
