// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FM DX log: learning the local stations first, a foreign PI raises one alert, the country comes from the ECC or is shown as likely.
#include "dect2/fm/fm_dx.h"
#include <cstdio>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    CHECK(rdsCountry(0xC201, 0xE2) == "LT", "LT");
    CHECK(rdsCountry(0x9201, 0xE3) == "LV", "LV");
    CHECK(rdsCountry(0x2201, 0xE4) == "EE", "EE");
    CHECK(rdsCountry(0xD301, 0xE0) == "DE", "DE");
    CHECK(rdsCountry(0xC201, -1).empty(), "no ECC");
    auto v = rdsCountriesLikely(0xC201);
    CHECK(v.size() == 5, "nibble C holds %zu countries", v.size());   // MT, GB, LT, HR, GE
    FmDxLog log; log.setLearnSeconds(100);
    for (int t = 0; t < 120; t += 10) { auto a = log.observe(1000 + t, 95.1, 0xC201, 0xE2, "LRT", 30); CHECK(!a.fire, "home station: no alert"); }
    CHECK(log.homeCountry() == "LT", "home learned: %s", log.homeCountry().c_str());
    // a foreign station with the ECC
    auto a = log.observe(1200, 101.3, 0x6345, 0xE1, "YLE X3M", 20);
    CHECK(a.fire && a.certain && a.country == "FI", "foreign with ECC");
    CHECK(!log.observe(1210, 101.3, 0x6345, 0xE1, "YLE X3M", 25).fire, "only once");
    // without an ECC: likely list, and it can be excluded only when home is not on the list
    a = log.observe(1220, 102.1, 0x3111, -1, "", 18);
    CHECK(a.fire && !a.certain && a.likely.find("PL") != std::string::npos, "likely list '%s'", a.likely.c_str());
    // a station whose nibble could be home: no alert without the ECC
    CHECK(!log.observe(1230, 103.0, 0xC777, -1, "", 18).fire, "could be home: quiet");
    // weak: below the level, no alert
    CHECK(!log.observe(1240, 104.0, 0x5777, 0xE0, "", 5).fire, "weak: quiet");
    // keep and restore
    FmDxLog b; b.load(log.serialize());
    CHECK(b.heard().size() == log.heard().size(), "reload");
    CHECK(b.learned(2000) == true, "learned after reload");
    CHECK(log.toCsv().find("FI") != std::string::npos, "csv");
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
