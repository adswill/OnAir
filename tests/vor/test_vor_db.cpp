// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Station list import (OurAirports and OpenAIP style CSV), ident matching, and the position from two radials with its error ellipse.
#include "dect2/vor/vor_db.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    // OurAirports navaids.csv layout (made-up stations), with quotes, an NDB and a DME to drop
    const std::string oa =
        "\"id\",\"filename\",\"ident\",\"name\",\"type\",\"frequency_khz\",\"latitude_deg\",\"longitude_deg\",\"elevation_ft\",\"iso_country\","
        "\"dme_frequency_khz\",\"dme_channel\",\"dme_latitude_deg\",\"dme_longitude_deg\",\"dme_elevation_ft\",\"slaved_variation_deg\",\"magnetic_variation_deg\",\"usageType\",\"power\",\"associated_airport\"\n"
        "1,\"a\",\"AAA\",\"Alpha, North\",\"VOR-DME\",\"113000\",\"25.0\",\"55.0\",\"10\",\"AE\",\"\",\"\",\"\",\"\",\"\",\"1.5\",\"2.0\",\"BOTH\",\"HIGH\",\"\"\n"
        "2,\"b\",\"BBB\",\"Bravo\",\"VOR\",\"116600\",\"25.5\",\"56.0\",\"10\",\"AE\",\"\",\"\",\"\",\"\",\"\",\"\",\"-3.0\",\"BOTH\",\"HIGH\",\"\"\n"
        "3,\"c\",\"CC\",\"Charlie\",\"NDB\",\"350\",\"25.2\",\"55.2\",\"10\",\"AE\",\"\",\"\",\"\",\"\",\"\",\"\",\"0\",\"\",\"\",\"\"\n"
        "4,\"d\",\"AAA\",\"Alpha far away\",\"VOR\",\"114500\",\"48.0\",\"11.0\",\"10\",\"DE\",\"\",\"\",\"\",\"\",\"\",\"\",\"3\",\"\",\"\",\"\"\n";
    std::vector<NavStation> db;
    std::string err;
    CHECK(navParseCsv(oa, db, err) == 3, "OurAirports: %zu read (%s)", db.size(), err.c_str());
    CHECK(db.size() == 3 && db[0].name == "Alpha, North" && std::fabs(db[0].freqMhz - 113.0) < 1e-9 && std::fabs(db[0].magVarDeg - 1.5) < 1e-9 && std::fabs(db[1].magVarDeg + 3) < 1e-9, "fields");
    // OpenAIP style: MHz, other names, semicolons
    const std::string oaip = "identifier;name;type;frequency;latitude;longitude;magneticDeclination\nIDXB;Dubai 12L;ILS;110.30;25.24;55.36;1.9\nDXB;Dubai;DVOR;114.1;25.25;55.37;2\n";
    CHECK(navParseCsv(oaip, db, err) == 2 && db.size() == 5, "OpenAIP style: %zu (%s)", db.size(), err.c_str());
    std::vector<NavStation> none;
    CHECK(navParseCsv("a,b,c\n1,2,3\n", none, err) == 0 && !err.empty(), "bad header");
    // matching
    const NavStation* s = navMatch(db, "AAA", 113.0);
    CHECK(s && s->name == "Alpha, North", "AAA on 113.0");
    CHECK(navMatch(db, "AAA", 0) == nullptr, "AAA without frequency is ambiguous");
    CHECK(navMatch(db, "aaa", 114.5) && navMatch(db, "AAA", 114.5)->country == "DE", "AAA on 114.5");
    CHECK(navMatch(db, "BBB", 0) && navMatch(db, "BBB", 113.0) == nullptr, "BBB");
    CHECK(navMatch(db, "IDXB", 110.3) && navMatch(db, "DXB", 114.1), "ILS and DVOR");

    // the fix: a position 60 km and 50 km from two stations
    const double plat = 25.6, plon = 55.4;
    const double b1 = navBearingDeg(25.0, 55.0, plat, plon), b2 = navBearingDeg(25.5, 56.0, plat, plon);
    NavFix f = navFix(25.0, 55.0, b1, 1.0, 25.5, 56.0, b2, 1.0);
    const double miss = navDistanceM(f.lat, f.lon, plat, plon);
    printf("fix %.5f %.5f, miss %.0f m, 1-sigma %.0f x %.0f m at %.0f deg, crossing %.0f deg\n", f.lat, f.lon, miss, f.sigMajorM, f.sigMinorM, f.majorAzDeg, f.crossDeg);
    CHECK(f.ok && miss < 300, "fix miss %.0f m", miss);
    // 1 degree at ~75 km is 1.3 km across each radial: the ellipse is of that order
    CHECK(f.sigMinorM > 500 && f.sigMajorM < 6000 && f.sigMajorM >= f.sigMinorM, "ellipse %.0f x %.0f", f.sigMajorM, f.sigMinorM);
    // a 1 degree error on radial 1 moves the fix by about the ellipse size
    NavFix g = navFix(25.0, 55.0, b1 + 1, 1.0, 25.5, 56.0, b2, 1.0);
    const double mv = navDistanceM(f.lat, f.lon, g.lat, g.lon);
    CHECK(g.ok && mv > 0.5 * f.sigMinorM && mv < 2.5 * f.sigMajorM, "1 deg moves %.0f m", mv);
    CHECK(!navFix(25.0, 55.0, 90, 1, 25.0, 56.0, 90, 1).ok, "parallel");
    CHECK(!navFix(25.0, 55.0, 270, 1, 25.5, 56.0, b2, 1).ok, "pointing away");
    double la, lo;
    navDestination(25.0, 55.0, 45, 100e3, la, lo);
    CHECK(std::fabs(navDistanceM(25.0, 55.0, la, lo) - 100e3) < 1 && std::fabs(navBearingDeg(25.0, 55.0, la, lo) - 45) < 0.01, "destination");
    printf(fails ? "vor db: %d FAILED\n" : "vor db: all passed\n", fails);
    return fails ? 1 : 0;
}
