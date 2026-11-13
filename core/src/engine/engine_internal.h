// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Small helpers shared by the Engine member files (engine*.cpp). Private to core/src/engine/: not installed, not part of the API.
#pragma once
#include <chrono>
#include <cstdint>

namespace dect2 {

// The receiver that runs for an engine standard code (setStandard): 0 and 1 are DVB-T2, 2 DVB-T, ... so it is the code minus one
inline int activeStdFor(int stdMode) { return stdMode >= 2 ? stdMode - 1 : 0; }

// The steady clock in nanoseconds (delivery watchdog and hold timers)
inline int64_t steadyNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

}   // namespace dect2
