// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Plain-text ACARS formats: position, OOOI, ETA, fuel, delay, fault and loadsheet messages by label and preamble. The layouts come from the published
// plugin list and example messages of airframesio/acars-decoder-typescript (MIT, see THIRD_PARTY.md); no code of it is used. Airlines differ, so a text
// that does not match every field of a layout gives nothing rather than half a reading.
#pragma once
#include <string>
#include <vector>

namespace dect2 {

struct AcarsReading {
    std::string kind;                 // "Position report", "OOOI: off", "Fault report", ...
    int category = 0;                 // AcarsReadingCategory
    std::vector<std::pair<std::string, std::string>> fields;
    bool hasPos = false; double lat = 0, lon = 0;
    int altFt = -1, headingDeg = -1;
    std::string from, to, flight;
};
enum AcarsReadingCategory { ACARS_READ_OTHER = 0, ACARS_READ_POSITION, ACARS_READ_OOOI, ACARS_READ_ETA, ACARS_READ_FAULT, ACARS_READ_ROUTE, ACARS_READ_LOADSHEET, ACARS_READ_WEATHER, ACARS_READ_TEXT };

// The formats above. False when the label or the text fits none of them.
bool acarsInterpret(const std::string& label, const std::string& text, AcarsReading& out);
// "Position report: 51.470, -0.454, FL360, EGLL to KJFK, ETA 14:30": what the tables show, "" when acarsInterpret returned false
std::string acarsInterpretLine(const std::string& label, const std::string& text);

} // namespace dect2
