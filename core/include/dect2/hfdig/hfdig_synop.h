// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SYNOP (FM 12, land stations, "AAXX") and SHIP (FM 13, "BBXX") weather reports in the text of an RTTY broadcast (for example the 50 baud
// bulletins of weather services), read into fields. Section 1 only (wind, temperature, dew point, pressure, cloud cover, and for a ship its position
// and sea temperature): the regional groups after 333 and 555 are left alone. A group that does not read is skipped, never guessed.
#pragma once
#include <string>
#include <vector>

namespace dect2 {
namespace hfdig {

struct WxReport {
    bool ship = false;            // BBXX (a ship or buoy) as against AAXX (a land station)
    std::string id;               // WMO station number (land) or call sign (ship)
    int day = 0, hour = 0;        // UTC, from the YYGG group
    bool hasPos = false; double lat = 0, lon = 0;   // ships only
    bool hasWind = false; int windDeg = 0; double windKn = 0;       // wind speed in knots whatever the unit of the report
    bool hasTemp = false; double tempC = 0;
    bool hasDew = false; double dewC = 0;
    bool hasSeaTemp = false; double seaC = 0;                        // ships, from the 0sTTT group of section 2
    bool hasPress = false; double stationHpa = 0;
    bool hasSlp = false; double slpHpa = 0;
    int oktas = -1;               // cloud cover 0..8, 9 sky not visible, -1 unknown
};

// every report in the text, in order. Reports must end with "=" (as bulletins do) to be read.
std::vector<WxReport> parseWxReports(const std::string& text);
// one line for a report: "SYNOP 10382  20th 12 UTC  wind 270 deg 12 kn  8.5 C  ..." (SHIP with the position)
std::string wxLine(const WxReport& r);

}  // namespace hfdig
}  // namespace dect2
