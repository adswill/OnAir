// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FSQ receiver telemetry, shared by the receiver and the UI (the common part is Kbd33Telemetry, kbd33_station.h).
#pragma once
#include "dect2/fsq/kbd33_station.h"

namespace dect2 {

struct FsqTelemetry : Kbd33Telemetry {};

inline std::string fsqSummary(const FsqTelemetry& t) { return kbd33Summary(t, "FSQ"); }

} // namespace dect2
