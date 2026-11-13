// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The "Photo / info" menu: opens the browser at a public aircraft or ship database. Never fetches anything itself.
#pragma once
#include <cstdint>
#include <string>

namespace photolinks {
// a small button with a drop-down (use in a detail panel); and the same items for a right-click menu (call inside BeginPopupContextItem)
void aircraftButton(uint32_t icao, const std::string& registration, const std::string& callsign);
void aircraftMenuItems(uint32_t icao, const std::string& registration, const std::string& callsign);
void shipButton(uint32_t mmsi, uint32_t imo, const std::string& name);
void shipMenuItems(uint32_t mmsi, uint32_t imo, const std::string& name);
}
