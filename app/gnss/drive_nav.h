// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GNSS > Navigate: the decoded fix as a car navigation position (route, next waypoint, ETA, off-route, trip log). Maths in dect2/geo/nav.h.
#pragma once
#include "../app.h"

void driveNavTick(App& a);                 // every frame while the GNSS mode runs: dead reckoning, trip log, prompts
void driveNavView(App& a, ImVec2 size);   // the screen
