// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The one station location (latitude, longitude, height, locator, name) every screen uses as its default (issue #453). Pure helpers:
// text that is a position (a locator or coordinates), the first saved value that wins when older per-screen copies are folded into the
// global one, and the test for "this screen keeps its own locator". Links maidenhead.cpp.
#pragma once
#include "dect2/geo/maidenhead.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

namespace dect2 { namespace station {

struct Candidate {
    std::string source;      // for the log: the preference the value came from
    double lat = 0, lon = 0; // used when text is empty
    std::string text;        // a locator or coordinates (wins over lat/lon when not empty)
};

struct Pick {
    bool found = false;
    double lat = 0, lon = 0;
    std::string source;
};

inline bool validLatLon(double lat, double lon) {
    return std::isfinite(lat) && std::isfinite(lon) && lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180;
}

// Nothing is set when both are zero (the old placeholder) or out of range
inline bool isSetLatLon(double lat, double lon) { return validLatLon(lat, lon) && !(lat == 0 && lon == 0); }

// A position from typed text: coordinates ("54.687, 25.28", "54 41 13 N 25 16 48 E") or a Maidenhead locator (the centre of the square)
inline bool resolveText(const std::string& text, double& lat, double& lon) {
    std::string t = text;
    while (!t.empty() && std::isspace((unsigned char)t.front())) t.erase(t.begin());
    while (!t.empty() && std::isspace((unsigned char)t.back())) t.pop_back();
    if (t.empty()) return false;
    double la = 0, lo = 0;
    bool ok = fromMaidenhead(t, la, lo);
    if (!ok) ok = parseCoordinates(t, la, lo);
    if (!ok || !validLatLon(la, lo)) return false;
    lat = la; lon = lo;
    return true;
}

// The first candidate that holds a usable position wins (the order of the list is the priority); nothing is lost when several differ
// because the caller keeps the older values as that screen's own override (see isOwnLocator)
inline Pick firstNonEmpty(const std::vector<Candidate>& list) {
    Pick p;
    for (const Candidate& c : list) {
        double la = c.lat, lo = c.lon;
        if (!c.text.empty()) {
            if (!resolveText(c.text, la, lo)) continue;
        } else if (!isSetLatLon(la, lo)) continue;
        p.found = true; p.lat = la; p.lon = lo; p.source = c.source;
        return p;
    }
    return p;
}

// The locator of the station: 4, 6 or 8 characters
inline std::string locatorOf(double lat, double lon, int chars = 6) { return validLatLon(lat, lon) ? toMaidenhead(lat, lon, chars) : std::string(); }

inline std::string upper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

// A screen keeps its own locator when the stored text is a valid locator that is not the station's square at the same length
inline bool isOwnLocator(const std::string& stored, double lat, double lon) {
    if (stored.empty()) return false;
    double la, lo;
    if (!fromMaidenhead(stored, la, lo)) return false;
    if (!isSetLatLon(lat, lon)) return true;
    return upper(stored) != upper(toMaidenhead(lat, lon, (int)stored.size()));
}

// The locator a screen uses: its own when it keeps one, else the station's
inline std::string effectiveLocator(bool own, const std::string& stored, double lat, double lon, int chars = 6) {
    if (own && !stored.empty()) return stored;
    return locatorOf(lat, lon, chars);
}

inline double clampHeightM(double m) { return std::isfinite(m) ? std::max(-500.0, std::min(9000.0, m)) : 0.0; }

}}
