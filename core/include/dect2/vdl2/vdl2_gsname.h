// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A readable name for a VDL2 ground station, from OnAir's own airport table (OurAirports, public domain): the airport it reports in its
// coverage parameter, else the nearest airport to its confirmed position. No third-party ground station list is used.
#pragma once
#include <cctype>
#include <string>
#include "dect2/airports/airports.h"
#include "dect2/vdl2/vdl2_tel.h"

namespace dect2 {

inline std::string vdl2AirportLabel(const Airport& a) {
    return a.icao + " " + (!a.city.empty() ? a.city : a.name);
}

// "EDDF Frankfurt" for a station reporting EDDF; "near EDDF Frankfurt" for the nearest airport within 150 km of its position; "" when neither.
inline std::string vdl2StationName(const Vdl2GroundStation& g, const AirportDb& db) {
    std::string tok;
    auto tryTok = [&](const std::string& t, std::string& out) {
        if (t.size() < 3 || t.size() > 4) return false;
        const Airport* a = db.find(t);
        if (!a) return false;
        out = vdl2AirportLabel(*a);
        return true;
    };
    std::string out;
    for (size_t i = 0; i <= g.airports.size(); i++) {
        const char c = i < g.airports.size() ? g.airports[i] : ' ';
        if (std::isalnum((unsigned char)c)) tok += (char)std::toupper((unsigned char)c);
        else { if (!tok.empty() && tryTok(tok, out)) return out; tok.clear(); }
    }
    if (g.hasPos) {
        const auto hits = db.nearby(g.lat, g.lon, 150.0, 1, false);
        if (!hits.empty()) return "near " + vdl2AirportLabel(db.all()[hits[0].index]);
    }
    return "";
}

}  // namespace dect2
