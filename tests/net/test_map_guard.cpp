// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The map guard (core/include/dect2/net/map_guard.h): no upload while transmitting and for 10 minutes after; nothing whose identity this app
// transmitted in the last 24 hours.
#include "dect2/net/map_guard.h"
#include "../common/test_util.h"
using namespace dect2::mapguard;

int main() {
    reset();
    CHECK(!holding(1000) && !transmitting(), "idle: nothing is held");
    txBegin(1000);
    CHECK(holding(1000) && holding(1000 + 5000) && transmitting() && holdLeftSec(2000) > 1e8, "held for as long as the app transmits");
    txEnd(1100);
    CHECK(holding(1100) && holding(1100 + 599) && !holding(1100 + 601), "held for 10 minutes after the end, then released");
    CHECK(holdLeftSec(1100 + 100) > 499 && holdLeftSec(1100 + 100) <= 500 && holdLeftSec(1100 + 700) == 0, "time left");
    txBegin(2000); txBegin(2010); txEnd(2020);
    CHECK(holding(2030 + 5000) && transmitting(), "two transmitters: held until both are quiet");
    txEnd(2100);
    CHECK(!transmitting() && holding(2100 + 599) && !holding(2100 + 601), "and 10 minutes after the last one");
    txEnd(2200);   // an unbalanced end does not go negative
    txBegin(9000);
    CHECK(transmitting(), "an extra end did not break the count");
    txEnd(9001);

    reset();
    CHECK(normalizeId(" ly1abc-9 ") == "LY1ABC-9" && normalizeId("a b/c") == "ABC", "ids are normalised");
    noteSent("LY1ABC-9", 100);
    CHECK(idBlocked("LY1ABC-9", 200) && idBlocked("ly1abc-9", 200) && idBlocked("LY1ABC", 200), "a transmitted callsign (and its base) is blocked");
    noteSent("LY1XYZ", 100);
    CHECK(idBlocked("LY1XYZ-5", 200), "the same callsign with another SSID is blocked");
    CHECK(!idBlocked("LY2ABC", 200) && !idBlocked("", 200), "another callsign is not");
    CHECK(idBlocked("LY1ABC", 100 + 24 * 3600 - 1) && !idBlocked("LY1ABC", 100 + 24 * 3600 + 5), "blocked for 24 hours, then forgotten");
    reset();
    noteSentAll(identitiesOf("", "", "MV TEST", 244123456u, "hello world"), 50);
    CHECK(idBlocked("244123456", 60), "a transmitted MMSI is blocked");
    CHECK(idBlocked("MV TEST", 60) && idBlocked("MVTEST", 60), "a transmitted ship name is blocked");
    char hex[16]; snprintf(hex, sizeof hex, "%06X", 244123456u & 0xFFFFFF);
    CHECK(idBlocked(hex, 60), "a transmitted address is blocked as the 6-digit ICAO form too");
    CHECK(idBlocked("WORLD", 60) && !idBlocked("hi", 60), "words of the text of 3 or more characters are identities");
    CHECK(!idBlocked("123456789", 60), "an unrelated id passes");
    noteSent("ab", 70);
    CHECK(!idBlocked("AB", 80), "two characters mean nothing");
    reset();
    CHECK(!holding(1) && !idBlocked("LY1ABC", 1), "reset");
    return testSummary("map_guard");
}
