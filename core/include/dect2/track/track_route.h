// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Route knowledge for the position estimator: an airport table, parsing of the route / ETA / OOOI / position lines of ACARS and VDL2 text
// where the formats are well defined, line-of-sight range from a ground station, and a small wind field from aircraft that report wind.
#pragma once
#include "track_geo.h"
#include <cstddef>
#include <string>
#include <vector>

namespace dect2::track {

struct Airport { const char* icao; float lat, lon; };
const Airport* airportTable(size_t& n);                 // sorted by ICAO code (hubs only: see docs/position-estimation.md)
const Airport* findAirport(const std::string& icao);    // nullptr when not in the table

// What the text of a message says about the flight.
struct RouteInfo {
    std::string origin, dest;                            // ICAO codes ("" unknown)
    int outSec = -1, offSec = -1, onSec = -1, inSec = -1, etaSec = -1;   // seconds of the UTC day, -1 unknown
    std::string nextWpt;                                 // name of the next waypoint when the text gives one
    bool any() const { return !origin.empty() || !dest.empty() || etaSec >= 0 || outSec >= 0 || offSec >= 0 || onSec >= 0 || inSec >= 0 || !nextWpt.empty(); }
};
// `label` is the ACARS label (may be ""), `text` the message text. Understood: the Q-label OOOI / airport layouts (as in
// acarsDecodeText), Honeywell/ARINC 702A flight-plan lines "FPN/RI:DA:KLAX:AA:KSFO", ICAO FPL strings "(FPL-...-EDDF1030 -...-OMDB0610 ...",
// "ETA 1234" / "/ETA1234Z" / "ETA/1234", "DEST KJFK" / "DST/KJFK" / "/DS KJFK", "NXT WPT ABCDE" / "NEXT WPT ABCDE". Returns true when any field was found.
bool parseRouteText(const std::string& label, const std::string& text, RouteInfo& out);

// A latitude/longitude written in a report, with the rounding step it was written with.
struct TextPosition { bool ok = false; double lat = 0, lon = 0, resolutionDeg = 0; int altFt = -1; };
// Formats: "N43312W123174" (DDMMm, tenths of a minute), "N38160W077075" (label 20: thousandths of a degree), "N 44.203,W 86.546" /
// "N 28.177/W 96.055" / "N51.4712 E000.4543" (decimal degrees), "N 51 28.3 E 000 27.1" (degrees, minutes with decimals).
// `labelHint` "20" selects thousandths for the 5/6 digit form; otherwise tenths of a minute.
TextPosition parseTextPosition(const std::string& labelHint, const std::string& text);

// Radio horizon: nautical miles to an aircraft at altFt from a ground antenna of antFt (4/3 Earth), the VHF line-of-sight bound.
double losRangeNm(double altFt, double antFt = 100);

// Winds aloft from aircraft that report (or derive) them; looked up for aircraft that do not.
class WindField {
public:
    void add(double t, double lat, double lon, double altM, double eMs, double nMs);   // wind toward east/north
    // inverse-distance weighted mean of the samples within maxKm, +-maxDz and maxAgeSec; false when none
    bool at(double t, double lat, double lon, double altM, double& eMs, double& nMs, double& sigmaMs) const;
    size_t size() const { return s_.size(); }
    double maxKm = 300, maxDzM = 1500, maxAgeSec = 1800;
private:
    struct S { double t, lat, lon, alt, e, n; };
    std::vector<S> s_;
    size_t head_ = 0;
};

}  // namespace dect2::track
