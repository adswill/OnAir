// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The DMR Map and Table sub-tabs: LRRP positions of the radios on a slippy map (tracks, filters)
// and a table of every position received, with CSV export. See dmr_map.cpp.
#pragma once

#include <cstdint>
struct App;

void dmrMap(App& a);
void dmrTable(App& a);
// select a radio on the map and centre on its last position (the Watch view's "Show on map")
void dmrMapFocus(uint32_t src, double lat, double lon);
