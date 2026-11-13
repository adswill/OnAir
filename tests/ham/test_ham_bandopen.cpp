// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// When is the band open (round 4 #38): 20 m open 06-17 UTC every day for 10 days, 40 m at night on 3 of them; the 7 and 30 day matrices
// count days, spots and the farthest; old days drop out; the saved text.
#include "dect2/ham/band_open.h"
#include "../common/test_util.h"
using namespace dect2::ham;

int main() {
    const int64_t d0 = 1'799'971'200;   // a midnight UTC
    BandOpenStore s;
    for (int d = 0; d < 10; d++) {
        for (int h = 6; h < 18; h++) for (int k = 0; k < 5; k++) s.add("20m", d0 + d * 86400 + h * 3600 + k * 600, 500 + h * 100 + d);
        if (d % 3 == 0) for (int h : {22, 23, 0, 1}) s.add("40m", d0 + d * 86400 + h * 3600 + 60, 1200);
    }
    const int64_t now = d0 + 9 * 86400 + 20 * 3600;
    auto m7 = s.matrix(now, 7), m30 = s.matrix(now, 30);
    CHECK(m7.count("20m") && m7["20m"][12].days == 7 && m7["20m"][12].spots == 35 && m7["20m"][3].days == 0 && m7["20m"][17].maxKm == 500 + 1700 + 9, "20 m, 7 days: %d days at 12 UTC", m7["20m"][12].days);
    CHECK(m30["20m"][12].days == 10 && m30["20m"][12].spots == 50, "30 days hold all 10");
    CHECK(m7.count("40m") && m7["40m"][23].days == 3 && m30["40m"][0].days == 4 && m30["40m"][12].days == 0, "40 m at night: %d / %d", m7["40m"][23].days, m30["40m"][0].days);
    CHECK(bandOrder("160m") < bandOrder("20m") && bandOrder("20m") < bandOrder("6m") && bandOrder("x") > bandOrder("6m"), "band order (ham_log.h)");
    BandOpenStore r; r.parse(s.serialize() + "bad\n1\tx\t99\t1\t1\n");
    CHECK(r.matrix(now, 30)["20m"][12].spots == 50 && r.matrix(now, 30)["40m"][23].days == 4, "round trip");
    r.add("20m", now + 40 * 86400, 100);
    CHECK(r.matrix(now + 40 * 86400, 30)["20m"][12].days == 0, "old days dropped");
    return testSummary("ham_bandopen");
}
