// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The one call the setup and the lessons make to set the gain automatically. The advanced auto-gain (branch autogain-030) provides
// autogainRun(App&, int how); autogain_firstrun.cpp maps this call onto it (Auto).
#pragma once
#include <string>

struct App;

// Find and set a good gain for this radio (index in App::devices) and mode (the mode id, as learn.cpp names it: "fm", "adsb", ...).
// Returns true when automatic gain is now running or a gain was set; false when the radio has no gain to set (a file, the demo signal).
bool autogainRun(App& a, int radio, const std::string& mode);
