// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The "Fast scan" scope of the merged Scanner view: the controls (profile presets, bands, threshold above the dynamic floor, recording), the
// live pseudo-spectrum with the floor, and the list of hits with one-click save to memory, lockout, priority and exclusion ranges.
// The scanning itself is core/src/fastscan (docs/fast-scan.md); this is the window part and the worker thread.
#pragma once
#include "../app.h"

namespace fastscanui {
bool running();
void stop();
void controls(App& a);   // the profile bar: preset, bands, thresholds, recording, start / stop
void body(App& a);       // spectrum, waterfall and the hits table
}  // namespace fastscanui
