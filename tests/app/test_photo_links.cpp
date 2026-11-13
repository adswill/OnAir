// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The database link builder: ids are checked and only [A-Za-z0-9-] reaches a URL.
#include "panels/photo_links.h"
#include <cstdio>
using namespace photolinks;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { fails++; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static bool has(const std::vector<Link>& v, const char* site, const std::string& url) { for (auto& l : v) if (std::string(l.site) == site && l.url == url) return true; return false; }
int main() {
    CHECK(cleanId(" d-abcd ") == "D-ABCD");
    CHECK(cleanId("").empty() && cleanId("   ").empty());
    CHECK(cleanId("A B").empty() && cleanId("A/B").empty() && cleanId("A?x=1").empty() && cleanId("A&b").empty() && cleanId("A%2F").empty());
    CHECK(cleanId("..N123").empty() && cleanId("A\n").empty() && cleanId(std::string("AB\0C", 4)).empty());
    CHECK(cleanId("ABCDEFGHIJKLM").empty() && cleanId("ABCDEFGHIJKL") == "ABCDEFGHIJKL");
    CHECK(hexOf(0x4CA2B1) == "4CA2B1" && hexOf(0x12) == "000012" && hexOf(0).empty() && hexOf(0x1000000).empty());
    auto a = aircraftLinks(0x4CA2B1, "ei-abc", "RYR12AB");
    CHECK(has(a, "Planespotters", "https://www.planespotters.net/hex/4CA2B1"));
    CHECK(has(a, "JetPhotos", "https://www.jetphotos.com/registration/EI-ABC"));
    CHECK(has(a, "ADS-B Exchange", "https://globe.adsbexchange.com/?icao=4CA2B1"));
    CHECK(has(a, "FlightAware", "https://www.flightaware.com/live/flight/RYR12AB"));
    CHECK(has(a, "Flightradar24", "https://www.flightradar24.com/data/aircraft/EI-ABC"));
    auto b = aircraftLinks(0, "N1/../x", "A B");
    CHECK(b.empty());
    auto c = aircraftLinks(0, "N123AB", "");
    CHECK(has(c, "Planespotters", "https://www.planespotters.net/search?q=N123AB") && has(c, "JetPhotos", "https://www.jetphotos.com/registration/N123AB"));
    auto d = aircraftLinks(0x1000001, "", "");
    CHECK(d.empty());
    for (auto& l : a) CHECK(l.url.find_first_of(" \"'<>\\\n") == std::string::npos);
    auto s = shipLinks(244123456, 9321483, "X");
    CHECK(has(s, "VesselFinder", "https://www.vesselfinder.com/vessels/details/9321483"));
    CHECK(has(s, "MarineTraffic", "https://www.marinetraffic.com/en/ais/details/ships/mmsi:244123456"));
    auto s2 = shipLinks(244123456, 0);
    CHECK(has(s2, "VesselFinder", "https://www.vesselfinder.com/?mmsi=244123456"));
    CHECK(shipLinks(0, 0, "").empty() && shipLinks(12345, 12, "A B").empty());
    CHECK(shipLinks(1000000000u, 0).empty());
    preferFirst(a, "FlightAware"); CHECK(std::string(a[0].site) == "FlightAware");
    preferFirst(a, "nope"); CHECK(std::string(a[0].site) == "FlightAware");
    std::printf(fails ? "FAILED\n" : "ok\n");
    return fails ? 1 : 0;
}
