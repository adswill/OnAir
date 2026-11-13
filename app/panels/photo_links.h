// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Links to public aircraft and ship databases (photos and details), built from the identifiers the program has.
// Nothing is fetched or embedded: choosing a link only opens the browser, so nothing leaves the program until then.
// Identifiers are checked and kept to [A-Za-z0-9-]; anything else gives no link (no text of a received message reaches a URL).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace photolinks {

struct Link { const char* site; std::string url; };

// upper-case copy of s when it is 1..maxLen characters of [A-Za-z0-9-] (surrounding blanks dropped), else empty
inline std::string cleanId(const std::string& s, size_t maxLen = 12) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\0')) e--;
    if (e == b || e - b > maxLen) return "";
    std::string o;
    for (size_t i = b; i < e; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return "";
        o += c;
    }
    return o;
}

// the 24-bit ICAO address as 6 hex digits; "" for 0 or more than 24 bits (not an ICAO address)
inline std::string hexOf(uint32_t icao) {
    if (!icao || (icao >> 24)) return "";
    static const char* d = "0123456789ABCDEF";
    std::string o(6, '0');
    for (int i = 5; i >= 0; i--, icao >>= 4) o[i] = d[icao & 15];
    return o;
}

// preferred = index into the returned list of the user's chosen site (see siteNames()); the chosen one is placed first
inline std::vector<Link> aircraftLinks(uint32_t icao, const std::string& registration, const std::string& callsign) {
    std::vector<Link> v;
    const std::string hex = hexOf(icao), reg = cleanId(registration, 10), cs = cleanId(callsign, 8);
    if (!hex.empty()) v.push_back({"Planespotters", "https://www.planespotters.net/hex/" + hex});
    else if (!reg.empty()) v.push_back({"Planespotters", "https://www.planespotters.net/search?q=" + reg});
    if (!reg.empty()) v.push_back({"JetPhotos", "https://www.jetphotos.com/registration/" + reg});
    if (!hex.empty()) v.push_back({"airport-data.com", "https://www.airport-data.com/aircraft/" + (reg.empty() ? hex : reg) + ".html"});
    if (!hex.empty()) v.push_back({"ADS-B Exchange", "https://globe.adsbexchange.com/?icao=" + hex});
    if (!cs.empty()) v.push_back({"FlightAware", "https://www.flightaware.com/live/flight/" + cs});
    if (!cs.empty() || !reg.empty()) v.push_back({"Flightradar24", "https://www.flightradar24.com/data/aircraft/" + (reg.empty() ? cs : reg)});
    return v;
}

// mmsi: 9 digits; imo: 7 digits (0 = unknown)
inline std::vector<Link> shipLinks(uint32_t mmsi, uint32_t imo, const std::string& name = "") {
    std::vector<Link> v;
    const bool okMmsi = mmsi >= 100000000u && mmsi <= 999999999u, okImo = imo >= 1000000u && imo <= 9999999u;
    if (okImo) v.push_back({"VesselFinder", "https://www.vesselfinder.com/vessels/details/" + std::to_string(imo)});
    else if (okMmsi) v.push_back({"VesselFinder", "https://www.vesselfinder.com/?mmsi=" + std::to_string(mmsi)});
    if (okMmsi) v.push_back({"MarineTraffic", "https://www.marinetraffic.com/en/ais/details/ships/mmsi:" + std::to_string(mmsi)});
    const std::string q = okImo ? std::to_string(imo) : okMmsi ? std::to_string(mmsi) : cleanId(name, 20);
    if (!q.empty()) v.push_back({"ShipSpotting", "https://www.shipspotting.com/photos/gallery?" + std::string(okImo ? "imo=" : okMmsi ? "mmsi=" : "shipName=") + q});
    return v;
}

// the user's preferred site first (by name; unknown name leaves the order)
inline void preferFirst(std::vector<Link>& v, const std::string& site) {
    for (size_t i = 1; i < v.size(); i++)
        if (site == v[i].site) { Link l = v[i]; v.erase(v.begin() + (long)i); v.insert(v.begin(), l); return; }
}

}  // namespace photolinks
