// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// When the map-server start-up question is asked.
#include "map_ask.h"
#include <cstdio>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    CHECK(mapShouldAsk(false, false, "", "0.2.7", false), "a new user is asked");
    CHECK(mapShouldAsk(true, false, "", "0.2.7", false), "asked at every start even after consent");
    CHECK(!mapShouldAsk(true, true, "0.2.7", "0.2.7", false), "declined for this version: not asked");
    CHECK(!mapShouldAsk(false, true, "0.2.7", "0.2.7", false), "declined without consent: not asked");
    CHECK(mapShouldAsk(false, true, "0.2.6", "0.2.7", false), "declined in an older version: asked after the update");
    CHECK(mapShouldAsk(false, true, "", "0.2.7", false), "declined by an older app without a stored version: asked");
    CHECK(mapShouldAsk(true, false, "0.2.7", "0.2.7", false), "a stored version without the declined flag does not stop it");
    CHECK(!mapShouldAsk(false, false, "", "0.2.7", true), "never while transmitting");
    CHECK(!mapShouldAsk(true, true, "0.2.6", "0.2.7", true), "never while transmitting, also after an update");
    printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
