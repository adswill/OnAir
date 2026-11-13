// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DSC positions as tracks per MMSI, and their export to KML 2.2 (Google Earth, DX-Atlas) and GPX 1.1.
#pragma once
#include "marine_mmsi.h"
#include "marine_tel.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace marine {

struct DscFix {
    std::string mmsi;            // the station the position belongs to (the one in distress for relays and acknowledgements)
    double lat = 0, lon = 0;
    int64_t time = 0;            // unix seconds when heard
    bool distress = false;
    std::string note;            // kind and nature
};

// Checked calls that carry a position, oldest first; the same station at the same place and call is kept once.
std::vector<DscFix> dscFixes(const std::vector<DscCall>& calls);
// Adds the new fixes of `calls` to `all` (dedup by MMSI, position and time); returns how many were new. At most `cap` fixes are kept.
size_t dscAddFixes(std::vector<DscFix>& all, const std::vector<DscCall>& calls, size_t cap = 5000);

// `stations` (with positions) become placemarks of their own; names come from the ship table.
std::string dscKml(const std::vector<DscFix>& fixes, const ShipDb& db);
std::string dscGpx(const std::vector<DscFix>& fixes, const ShipDb& db);

} // namespace marine
} // namespace dect2
