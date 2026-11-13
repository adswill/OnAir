// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MMSI -> station kind and country (ais_mid.h) and the country filter (ais_alerts.h).
#include "dect2/ais/ais_alerts.h"
#include "dect2/ais/ais_mid.h"
#include <cstdio>
#include <cstring>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    auto is = [](uint32_t m, int kind, const char* iso) { const AisMmsiInfo i = aisMmsiInfo(m); return i.kind == kind && !strcmp(i.iso, iso); };
    CHECK(is(211123456, AIS_MMSI_SHIP, "DE"), "German ship");
    CHECK(is(244670123, AIS_MMSI_SHIP, "NL"), "Dutch ship");
    CHECK(is(277123456, AIS_MMSI_SHIP, "LT"), "Lithuanian ship");
    CHECK(is(366123456, AIS_MMSI_SHIP, "US"), "US ship");
    CHECK(is(2320001, AIS_MMSI_COAST, "GB"), "coast station 00232");
    CHECK(is(21100123, AIS_MMSI_GROUP, "DE"), "group 0211");
    CHECK(is(111232123, AIS_MMSI_SAR_AIRCRAFT, "GB"), "SAR aircraft 111232");
    CHECK(is(982110001, AIS_MMSI_AUXILIARY, "DE"), "auxiliary 98211");
    CHECK(is(992111234, AIS_MMSI_ATON, "DE"), "AtoN 99211");
    CHECK(is(823212345, AIS_MMSI_HANDHELD, "GB"), "handheld 8232");
    CHECK(is(970123456, AIS_MMSI_SART, "") && aisMmsiInfo(970123456).mid == 0, "AIS-SART has no country");
    CHECK(aisMmsiInfo(972123456).kind == AIS_MMSI_MOB && aisMmsiInfo(974123456).kind == AIS_MMSI_EPIRB, "MOB, EPIRB");
    CHECK(aisMmsiInfo(0).kind == AIS_MMSI_UNKNOWN && aisMmsiInfo(123).kind == AIS_MMSI_UNKNOWN && aisMmsiInfo(100000000).kind == AIS_MMSI_UNKNOWN, "not an MMSI of the scheme");
    CHECK(aisMmsiInfo(299123456).kind == AIS_MMSI_SHIP && !strcmp(aisMmsiInfo(299123456).iso, ""), "unallocated MID: ship, no country");
    CHECK(!strcmp(aisMmsiKindText(AIS_MMSI_ATON), "Aid to navigation"), "kind text");
    // table sanity: sorted, ISO codes two letters
    for (int m = 200; m < 800; m++) if (const char* c = aisMidIso(m)) CHECK(strlen(c) == 2 && aisMidCountry(m)[0], "mid %d row", m);
    // the country filter
    AisVessel de; de.mmsi = 211123456; de.cls = AIS_CLASS_A; de.shipType = 70;
    AisVessel nl = de; nl.mmsi = 244670123;
    AisVessel sart = de; sart.mmsi = 970123456;
    AisFilter f;
    CHECK(f.matches(de) && f.matches(nl) && f.matches(sart) && !f.active(), "no country filter: everything");
    f.countries = {"DE"};
    CHECK(f.active() && f.matches(de) && !f.matches(nl) && !f.matches(sart), "only DE");
    const AisFilter g = AisFilter::fromText(f.toText());
    CHECK(g.countries.size() == 1 && g.countries[0] == "DE" && g.matches(de), "country filter round trip: %s", f.toText().c_str());
    f.countries = {"DE", "NL"};
    CHECK(AisFilter::fromText(f.toText()).countries.size() == 2, "two countries");
    CHECK(AisFilter::fromText("classes=63;groups=1023;moving=0;sog=0.5").countries.empty(), "old setting text has no countries");
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ais mid OK\n");
    return 0;
}
