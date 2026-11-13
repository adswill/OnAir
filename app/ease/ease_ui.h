// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Small shared pieces of the first-use helpers (docs/EASE2-3.8.md).
#pragma once
#include "app.h"

namespace easeui {

// A geostationary satellite chosen by position: at most once, only while the user has not chosen one or tuned elsewhere.
struct SatPick { double prevMhz = 0; bool undoable = false; };
// Returns the index picked on this call (the caller tunes to it), else -1. prefKey is the saved satellite choice of the mode.
int preselectSat(App& a, const char* prefKey, const double* slotLon, int count, SatPick& st);
// "Point the antenna south (173 deg), 27.6 deg up" for the chosen slot; with Undo right after an automatic pick. Returns true when Undo was pressed.
bool satPointing(App& a, double slotLonDeg, SatPick& st);

// "Auto band" for the sensor modes. Returns true when the manual band controls should be shown (auto is off).
bool autoBandControl(App& a, const char* prefKey, uint64_t decoded);

// "Repeaters near me" for a voice mode ("DMR", "D-STAR", "Fusion", "M17"): a button and its list. The list is one the user imported.
void repeaterPicker(App& a, const char* mode);

// DVB-S and DVB-C: the transponders the network announces about itself, once the stream is locked. Returns true when a row was tuned;
// pol then holds the satellite polarisation of that row (0 horizontal, 1 vertical, -1 none).
bool nitTransponders(App& a, int kind, bool locked, bool inverted, int& pol);

// "Align": a big level bar with peak hold for aiming a dish, from one frame's level in dB over the noise. The tone is off until ticked and is never remembered.
void alignButton(App& a, const char* id, float db, bool valid);

// PSK Reporter / WSPRnet / ham setup: fills a blank callsign and locator once per field, from the others and the station position. A field the user cleared stays clear.
bool stationFillOnce(App& a, const char* doneKey, std::string& call, std::string& locator);

// A voice mode whose codec is not installed: said on the mode's own screen before the first call, with the install guide; Hide is remembered per mode.
void codecNotice(App& a);

}
