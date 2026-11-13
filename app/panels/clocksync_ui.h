// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The computer clock against GPS or a time station: the "Sync computer clock" section (the measured offset, the source, Sync now) and the optional
// hobby NTP server, shared by the Time signals and the GNSS screens (time_clocksync.cpp).
#pragma once
#include "../app.h"
#include <vector>

void clockPublishGps(const App& a);      // every frame while GNSS is selected: the offset to the shared ClockDiscipline
void clockPublishStation(const App& a);  // the same for the Time signals mode
void clockSyncPanel(App& a);             // source, offset, Sync now, command to copy, the NTP server switch and its counters
void clockPlot(const char* id, const char* label, const std::vector<float>& v, float lo, float hi, const char* unit);
void clockMinMax(const std::vector<float>& v, float& lo, float& hi, float pad);
