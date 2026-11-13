// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// WSPR balloon telemetry formulas against the S4 page's worked example (altitude, locator letters, GPS flags); simulated values otherwise.
#include "dect2/hfdig/hfdig_balloon.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"
#undef CHECK
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main() {
    auto b = wsprBalloonDecode("0C0QQE", "RG74", 43);
    CHECK(b.ok && b.sub == "IQ" && b.altM == 80 && b.speedKt == 0 && b.gpsOk && b.sats8);
    printf("%s (page example says 36 C 3.8 V)\n", wsprBalloonText(b).c_str());
    CHECK(!wsprBalloonDecode("G4ABCD", "IO91", 37).ok);
    CHECK(!wsprBalloonDecode("0C0QQE", "RG74", 5).ok);       // not a listed power
    CHECK(!wsprBalloonDecode("0C0QQ", "RG74", 43).ok);
    return testSummary("hfdig_balloon");
}
