// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The modes added after FM, seen together: one status line per telemetry (the command line prints it).
#pragma once
#include "mode_tuning.h"
#include "dect2/t2/t2rx.h"
#include <string>

namespace dect2 {
std::string modeSummary(const RxTelemetry& t);   // "" when the telemetry is not from one of these modes
}
