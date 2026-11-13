// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The NAVTEX table: every row sane, slots and the distance order
#include "dect2/ease/navtex.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace dect2::ease;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int testNavtex() {
    CHECK(kNavtexStationCount > 180);
    for (int i = 0; i < kNavtexStationCount; i++) {
        const NavtexStation& s = kNavtexStations[i];
        CHECK(s.letter >= 'A' && s.letter <= 'X');
        CHECK(s.khz == 518 || s.khz == 490);
        CHECK(s.lat >= -90 && s.lat <= 90 && s.lon >= -180 && s.lon <= 180);
        CHECK(s.slotMin >= -1 && s.slotMin < 240);
        CHECK(s.name && *s.name);
        CHECK(s.navarea >= 1 && s.navarea <= 21);
    }
    NavtexStation s{1, 'F', 518, "x", 0, 0, -1};
    CHECK(navtexSlotMinute(s) == 50);                  // the letter rule: F = 00:50
    const int64_t day = 20000 * 86400LL;
    CHECK(navtexSecsToNext(s, day) == 50 * 60);
    CHECK(navtexSecsToNext(s, day + 50 * 60) == 0);
    CHECK(navtexSecsToNext(s, day + 50 * 60 + 1) == 4 * 3600 - 1);
    CHECK(navtexSecsToNext(s, day + 4 * 3600 + 50 * 60) == 0);
    for (int64_t t = day; t < day + 86400; t += 37) { const int n = navtexSecsToNext(s, t); CHECK(n >= 0 && n < 4 * 3600); }
    NavtexStation l{3, 'A', 518, "x", 0, 0, 180};      // a station off the letter rule (a listed 03:00)
    CHECK(navtexSlotMinute(l) == 180);

    auto nearL = navtexNearest(54.69, 25.28, 518, day);   // Vilnius
    CHECK(!nearL.empty());
    for (size_t i = 1; i < nearL.size(); i++) CHECK(nearL[i - 1].km <= nearL[i].km);
    CHECK(nearL.size() >= 3);
    CHECK(!strcmp(nearL[0].st->name, "Tallinn"));
    CHECK(nearL[0].km > 500 && nearL[0].km < 560);
    auto farL = navtexNearest(-77.0, 0.0, 490, day);      // Antarctica: nothing in range, the single nearest
    CHECK(farL.size() == 1);

    const NavtexStation* b = navtexByLetter('B', 518, true, 45.0, 34.0);   // B is both Algiers and Kerch: the near one
    CHECK(b && !strcmp(b->name, "Kerch"));
    b = navtexByLetter('B', 518, true, 37.0, 3.0);
    CHECK(b && !strcmp(b->name, "Algiers"));
    CHECK(navtexByLetter('B', 518, false, 0, 0) == nullptr);   // ambiguous without a position
    CHECK(navtexByLetter('Y', 518, true, 0, 0) == nullptr);
    return fails;
}
