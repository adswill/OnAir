// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The one station location (Settings > Location): latitude, longitude, height, locator, name. Screens use it as their default; a screen
// that keeps its own locator shows "using the global location" with a "change" link (issue #453, docs/STATION-LOCATION.md).
#pragma once
#include <string>

struct App;

namespace stationloc {

// Once at start-up, before the preferences are read by the screens: folds the older per-screen positions into the global one (first
// non-empty wins) and marks the locators that differ from it as the screen's own, so nothing is lost
void migrate();

std::string locator();                         // 6 characters, "" when the station position is not set
std::string name();                            // Settings > Location > Station name
bool setFromText(App& a, const std::string&);  // a locator or coordinates typed by the user: sets latitude and longitude
void propagate(App& a);                        // after the global position changed: the screens that follow it get the new locator

// A locator field of a screen. Follows the global location until the user chooses another; returns true when `value` changed
bool locatorRow(const char* label, std::string& value, const char* ownKey, float width);

}
