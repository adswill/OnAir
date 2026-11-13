// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The imported repeater list: common CSV shapes, bad rows, mode filter, nearest first
#include "dect2/ease/repeaters.h"
#include <cstdio>

using namespace dect2::ease;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int testRepeaters() {
    int skipped = 0; std::string err;
    const char* a = "Output Freq,Input Freq,Callsign,Nearest City,Lat,Long,Mode,Color Code\r\n"
                    "438.200,430.600,LY1AA,Vilnius,54.69,25.28,DMR,1\r\n"
                    "145.650,145.050,LY2BB,\"Kaunas, centre\",54.90,23.89,D-STAR,\r\n"
                    "439.100,431.500,YL3CC,Riga,56.95,24.11,YSF / C4FM,\r\n"
                    "433.000,433.000,XX,Nowhere,,,DMR,3\r\n"
                    "bad,row,here,,,,,\r\n";
    auto v = parseRepeaterCsv(a, skipped, err);
    CHECK(err.empty()); CHECK(v.size() == 3); CHECK(skipped == 2);
    CHECK(v[0].call == "LY1AA"); CHECK(v[0].colourCode == 1); CHECK(v[1].where == "Kaunas, centre"); CHECK(v[1].colourCode == -1);
    auto d = nearestRepeaters(v, "DMR", 54.7, 25.3);
    CHECK(d.size() == 1); CHECK(d[0].r->call == "LY1AA"); CHECK(d[0].km < 5);
    CHECK(nearestRepeaters(v, "D-STAR", 54.7, 25.3).size() == 1);
    CHECK(nearestRepeaters(v, "Fusion", 56.9, 24.1).size() == 1);
    CHECK(nearestRepeaters(v, "M17", 54.7, 25.3).empty());
    CHECK(nearestRepeaters(v, "DMR", 10.0, 10.0).empty());   // out of range

    // no mode column: kept for every mode; nearest first; a locator instead of lat/lon; semicolons; Hz
    auto w = parseRepeaterCsv("frequency;locator;call\n438800000;KO24PQ;A1\n439000000;KO14AA;A2\n", skipped, err);
    CHECK(w.size() == 2); CHECK(w[0].mhz == 438.8);
    auto n = nearestRepeaters(w, "M17", 54.6, 25.3);
    CHECK(n.size() == 2); CHECK(n[0].r->call == "A1");

    CHECK(parseRepeaterCsv("foo,bar\n1,2\n", skipped, err).empty()); CHECK(!err.empty());
    CHECK(parseRepeaterCsv("", skipped, err).empty()); CHECK(!err.empty());
    // a colour code outside 0..15 is ignored, not trusted
    auto c = parseRepeaterCsv("freq,lat,lon,cc\n438.1,54,25,99\n", skipped, err);
    CHECK(c.size() == 1); CHECK(c[0].colourCode == -1);
    CHECK(c[0].mode.empty()); CHECK(repeaterMatchesMode(c[0], "DMR"));

    // Open Repeater answers: {"data":[...]}, a bare array, strings or numbers, MHz / kHz / Hz, bad rows, an error answer
    auto o = parseOpenRepeaterJson("{\"data\":[{\"callsign\":\"LY1AA\",\"frequency\":\"438.200\",\"city\":\"Vilnius \\u0161\",\"lat\":54.69,\"lng\":25.28,\"mode\":\"DMR\"},"
                                   "{\"callsign\":\"B\",\"frequency\":145650,\"lat\":\"54.9\",\"lng\":\"23.9\",\"mode\":\"D-STAR\"},"
                                   "{\"callsign\":\"C\",\"frequency\":439100000,\"lat\":56.95,\"lng\":24.11},"
                                   "{\"callsign\":\"D\",\"frequency\":438.1,\"lat\":null,\"lng\":1},{\"callsign\":\"E\",\"frequency\":438.1,\"lat\":0,\"lng\":0},{\"callsign\":\"F\"},7],\"page\":1}", skipped, err);
    CHECK(err.empty()); CHECK(o.size() == 3); CHECK(skipped == 4);
    CHECK(o[0].call == "LY1AA" && o[0].mhz == 438.2 && o[0].mode == "DMR" && o[0].where.size() > 7);
    CHECK(o[1].mhz > 145.64 && o[1].mhz < 145.66 && o[1].lat == 54.9); CHECK(o[2].mhz > 439.09 && o[2].mhz < 439.11);
    CHECK(nearestRepeaters(o, "DMR", 54.7, 25.3).size() == 2);   // the DMR one and the one with no mode text
    CHECK(parseOpenRepeaterJson("[{\"frequency\":433.5,\"lat\":1,\"lng\":2}]", skipped, err).size() == 1);
    CHECK(parseOpenRepeaterJson("{\"error\":\"daily limit reached\"}", skipped, err).empty() && err == "daily limit reached");
    CHECK(parseOpenRepeaterJson("<html>", skipped, err).empty() && !err.empty());
    CHECK(parseOpenRepeaterJson("{\"data\":[", skipped, err).empty() && !err.empty());
    CHECK(parseOpenRepeaterJson("{\"data\":[]}", skipped, err).empty() && err.empty());
    CHECK(openRepeaterSearchUrl(54.6872, 25.2797, 300, 0, 100) == "https://www.openrepeater.org/api/v1/search?lat=54.6872&lng=25.2797&radius=300&page=1&limit=100");
    return fails;
}
