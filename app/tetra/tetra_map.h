// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The TETRA Map sub-tab: the LIP position reports (short and long, TS 100 392-18-1) of the terminals on the shared slippy map, each ISSI with its last
// position and its track; a click shows the time and the message the position came from. Save results writes the positions as CSV, KML or GPX.
#pragma once
struct App;

void tetraMap(App& a);
