// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AIS arrivals board (round 4 #15): destination spellings (LOCODE with and without a space, a country prefix, ROADS / ANCH suffixes, one typo,
// FROM>TO, aliases) and refusals, the ETA without a year (and over New Year), the worked-out ETA from distance and speed, the order, the settings text.
#include "dect2/ais/ais_arrivals.h"
#include "../common/test_util.h"
#include <cmath>
using namespace dect2;

int main() {
    AisPort k; k.name = "Klaipeda"; k.locode = "LTKLJ"; k.aliases = {"KLP"}; k.hasPos = true; k.lat = 55.70; k.lon = 21.12;
    const char* yes[] = {"KLAIPEDA", "klaipeda", "LT KLJ", "LTKLJ", "LTKLJ01", "KLAIPEDA ROADS", "KLAIPEDA ANCH.", "LT KLAIPEDA", "KLAPEDA", "KLAIPEDS", "RIGA>KLAIPEDA", "SE STO > LT KLJ", "KLP", " K L A I P E D A "};
    const char* no[] = {"", "RIGA", "KLAIPEDA>RIGA", "FOR ORDERS", "LTKLX", "KL", "GDANSK", "KIEL", "KLJAIPEDA2X3"};
    for (const char* s : yes) CHECK(aisDestinationMatches(s, k), "matches '%s'", s);
    for (const char* s : no) CHECK(!aisDestinationMatches(s, k), "does not match '%s'", s);
    AisPort r; r.name = "Riga";
    CHECK(aisDestinationMatches("RIGA", r) && !aisDestinationMatches("RIGB", r) && aisDestinationMatches("RIGA ANCHORAGE", r), "short names: exact or prefix only");

    const int64_t now = 1'800'000'000;   // 2027-01-15 08:00 UTC
    CHECK(aisEtaUtc(1, 16, 6, 30, now) == now + 86400 - 5400, "tomorrow 06:30");
    CHECK(aisEtaUtc(1, 15, 2, 0, now) == now - 6 * 3600, "earlier today stays today (a late ship)");
    CHECK(aisEtaUtc(12, 31, 23, 0, 1'798'675'200 + 3600) == 1'798'758'000, "31 Dec this year");
    CHECK(aisEtaUtc(1, 2, 12, 0, 1'798'675'200 + 86400 * 364) > 1'798'675'200 + 86400 * 364, "after New Year: next January");
    CHECK(aisEtaUtc(0, 0, 24, 60, now) == 0 && aisEtaUtc(1, 16, 24, 60, now) == aisEtaUtc(1, 16, 0, 0, now), "not given; hour 24 = day only");

    std::vector<AisVessel> vs(5);
    vs[0].mmsi = 1; vs[0].name = "LATE"; vs[0].destination = "LTKLJ"; vs[0].etaMonth = 1; vs[0].etaDay = 17; vs[0].etaHour = 10; vs[0].etaMin = 0;
    vs[1].mmsi = 2; vs[1].name = "SOON"; vs[1].destination = "KLAIPEDA"; vs[1].etaMonth = 1; vs[1].etaDay = 15; vs[1].etaHour = 12; vs[1].etaMin = 0;
    vs[1].hasPos = true; vs[1].lat = 55.70 + 18.52 / 111.2; vs[1].lon = 21.12; vs[1].sog = 10;   // 18.52 km north at 10 kn: 1 h
    vs[2].mmsi = 3; vs[2].name = "NOETA"; vs[2].destination = "KLAIPEDA ROADS"; vs[2].hasPos = true; vs[2].lat = 55.70; vs[2].lon = 21.12 + 0.2; vs[2].sog = 0.1f;
    vs[3].mmsi = 4; vs[3].name = "OTHER"; vs[3].destination = "RIGA";
    vs[4].mmsi = 5; vs[4].name = "NOPOS"; vs[4].destination = "LT KLJ";
    const auto b = aisArrivals(vs, k, now);
    CHECK(b.size() == 4 && b[0].mmsi == 2 && b[1].mmsi == 1, "board: 4 vessels, by declared ETA (%zu)", b.size());
    if (b.size() == 4) {
        CHECK(std::fabs(b[0].distKm - 18.52) < 0.2 && std::llabs(b[0].etaWorked - (now + 3600)) < 60, "worked-out ETA %.2f km, %+lld s", b[0].distKm, (long long)(b[0].etaWorked - now));
        CHECK(b[2].etaDeclared == 0 && b[3].etaDeclared == 0 && b[2].etaWorked == 0 && b[2].distKm > 12 && b[2].distKm < 13 && b[3].distKm < 0, "no ETA: last; a ship at anchor has no worked ETA");
    }
    AisPort back = aisPortFromText(aisPortToText(k));
    CHECK(back.name == "Klaipeda" && back.locode == "LTKLJ" && back.hasPos && std::fabs(back.lat - 55.70) < 1e-6 && back.aliases.size() == 1 && back.aliases[0] == "KLP", "settings round trip");
    AisPort np; np.name = "X|Y";
    CHECK(!aisPortFromText(aisPortToText(np)).hasPos && aisPortFromText(aisPortToText(np)).name == "X Y", "no position, separators cleaned");
    return testSummary("ais_arrivals");
}
