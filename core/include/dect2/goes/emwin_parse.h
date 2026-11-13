// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// EMWIN bulletins (NOAA/NWS text products relayed on GOES HRIT): the WMO heading, the AWIPS product id, VTEC event codes, UGC areas and warning polygons, so a product
// list can show what a bulletin is and where it applies. The text formats are NWS Directive 10-1701 (VTEC), 10-1702 (UGC) and FMH-1; only plain text is read.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

struct EmwinVtec { std::string action, office, phenomenon, significance; int eventNo = 0; std::string begin, end; };
struct EmwinBulletin {
    bool ok = false;
    std::string wmoHeader;       // "WUUS53 KTOP 091830"
    std::string station;         // "KTOP"
    std::string awips;           // "TORTOP"
    std::string productCategory; // "TOR"
    std::vector<EmwinVtec> vtec;
    std::vector<std::string> ugc;            // expanded codes: "KSZ040", "KSC091"
    std::vector<std::pair<double, double>> polygon;   // lat, lon in degrees (west is negative)
    std::string headline;        // first line in capitals after the header ("TORNADO WARNING")
    int severity = 0;            // 0 other, 1 advisory or statement, 2 watch, 3 warning (from VTEC significance, else the product id)
};

EmwinBulletin emwinParse(const std::string& text);

}  // namespace dect2
