// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The near-me navaid list: the real column layout of OurAirports navaids.csv, including quoted names and a DME's own frequency.
#include "dect2/vor/navaid_list.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
static int fails = 0;
#define CHECK(c, w) do { if (!(c)) { printf("FAIL: %s\n", w); fails++; } } while (0)

static const char kCsv[] =
    "id,filename,ident,name,type,frequency_khz,latitude_deg,longitude_deg,elevation_ft,iso_country,dme_frequency_khz,dme_channel,dme_latitude_deg,dme_longitude_deg,dme_elevation_ft,slaved_variation_deg,magnetic_variation_deg,usageType,power,associated_airport\n"
    "1,x,VIL,\"Vilnius, VOR\",VOR-DME,116300,54.64,25.28,500,LT,1100000,110X,54.64,25.28,500,5,5,BOTH,HIGH,EYVI\n"
    "2,x,KUN,Kaunas,NDB,335,54.97,24.09,0,LT,,,,,,,,,,EYKA\n"
    "3,x,PAL,Palanga,DME,114400,55.97,21.09,0,LT,1187000,88X,55.97,21.09,0,,,,,EYPA\n"
    "4,x,RIX,Riga,VOR,112300,56.92,23.97,0,LV,,,,,,,,,,EVRA\n"
    "5,x,NOPE,Bad row,NDB,300,,,0,LT,,,,,,,,,,\n"
    "6,x,TOOFAR,Tokyo,VOR,113000,35.5,139.8,0,JP,,,,,,,,,,RJTT\n"
    "7,x,X,Weird,LOCATOR,300,1,1,0,LT,,,,,,,,,,\n";

int main() {
    std::vector<Navaid> db; std::string err;
    const int n = navaidParseCsv(kCsv, db, err);
    CHECK(n == 5, "five usable stations (bad position and unknown type dropped)");
    CHECK(db[0].name == "Vilnius, VOR" && db[0].kind == NavaidVor && std::fabs(db[0].freqMhz - 116.3) < 1e-6, "quoted name, VOR in MHz");
    CHECK(db[1].kind == NavaidNdb && std::fabs(db[1].freqMhz - 0.335) < 1e-6, "NDB: kHz to MHz");
    CHECK(db[2].kind == NavaidDme && std::fabs(db[2].freqMhz - 1187.0) < 1e-6, "DME: its own frequency");
    { std::vector<Navaid> d2; std::string e; CHECK(navaidParseCsv("not,a,csv\n1,2,3\n", d2, e) == 0 && !e.empty(), "wrong file says so"); }
    { std::vector<Navaid> d2; std::string e; CHECK(navaidParseCsv("", d2, e) == 0 && !e.empty(), "empty file"); }
    const double lat = 54.69, lon = 25.28;
    auto nearL = navaidNearest(db, lat, lon, NavaidVor, 500, 10);
    CHECK(nearL.size() == 2 && db[nearL[0].index].ident == "VIL" && db[nearL[1].index].ident == "RIX", "VOR list sorted, Tokyo out of range, NDB filtered");
    CHECK(nearL[0].km < nearL[1].km && nearL[0].km < 10, "sorted by distance");
    CHECK(navaidNearest(db, lat, lon, NavaidNdb | NavaidDme, 500, 10).size() == 2, "kinds combined");
    CHECK(navaidNearest(db, lat, lon, NavaidVor, 500, 1).size() == 1, "limit");
    int a = -1, b = -1; double x = 0;
    std::vector<NavaidNear> pts = {{0, 10, 10}, {1, 20, 100}, {2, 30, 185}};
    CHECK(navaidBestCrossing(pts, a, b, &x) && a == 0 && b == 1 && std::fabs(x - 90) < 1e-9, "pair crossing at 90 wins");
    std::vector<NavaidNear> pts2 = {{0, 10, 350}, {1, 20, 5}, {2, 30, 170}};   // 350 vs 170 cross at 0: useless
    CHECK(navaidBestCrossing(pts2, a, b, &x) && std::fabs(x - 15) < 1e-9 && a + b == 1, "angles wrap around north; 180 apart is no fix");
    std::vector<NavaidNear> one = {{0, 1, 1}};
    CHECK(!navaidBestCrossing(one, a, b), "needs two");
    if (!fails) printf("vor navaids: all passed\n");
    return fails ? 1 : 0;
}
