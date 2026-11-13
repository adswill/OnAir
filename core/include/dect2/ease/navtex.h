// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NAVTEX stations (letter and NAVAREA to name and place) and their four-hourly slots. Pure, no I/O.
// The table is a community list and stations change: a name is a hint, the letter on the air is the fact.
#pragma once
#include <cstdint>
#include <vector>

namespace dect2::ease {

struct NavtexStation { int navarea; char letter; int khz; const char* name; double lat, lon; int slotMin; };   // slotMin: 0..239 after 00:00 UTC, -1 not listed
extern const NavtexStation kNavtexStations[];
extern const int kNavtexStationCount;

// Minutes into the four-hour cycle of the station's broadcast (its listed slot, or A 00:00, B 00:10 ... X 03:50 when none is listed).
int navtexSlotMinute(const NavtexStation& s);
// Seconds to the start of its next broadcast (0..14399; 0 = starting now).
int navtexSecsToNext(const NavtexStation& s, int64_t unixUtc);

struct NavtexNear { const NavtexStation* st = nullptr; double km = 0; double bearing = 0; int secsToNext = 0; };
// Stations on this frequency, nearest first: those within maxKm, and if none is, the single nearest one.
std::vector<NavtexNear> navtexNearest(double lat, double lon, int khz, int64_t unixUtc, int maxCount = 6, double maxKm = 1500);
// The station behind a letter heard on this frequency: the nearest with that letter, or the only one in the table without a position. nullptr when unknown.
const NavtexStation* navtexByLetter(char letter, int khz, bool havePos, double lat, double lon);

} // namespace dect2::ease
