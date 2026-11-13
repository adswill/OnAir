// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The whole built-in mode registry (modes.cpp table()), for checks over every mode (tests/app/test_mode_numbers.cpp).
// Kept out of mode_tuning.h so that widely included header stays as it is.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include <vector>

namespace dect2 {
const std::vector<ModeTuning>& modeTunings();   // every built-in mode, one entry per engine standard
}
