// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Band hopping (round 4 #17): the coordinated 20-minute cycle, a band switched off (the next one in the table), bands outside the table,
// the plain rotation, hours with different bands and with none, slot edges and negative times, the settings text.
#include "dect2/hfdig/hfdig_hop.h"
#include "../common/test_util.h"
using namespace dect2;

int main() {
    const int64_t day = 1'799'971'200;   // 2027-01-15 00:00 UTC
    HopPlan all = HopPlan::allHours(0x3FF);   // 160 .. 10 m
    for (int m = 0; m < 40; m += 2) CHECK(hopBand(all, day + m * 60) == (m / 2) % 10, "coordinated minute %d -> %d", m, hopBand(all, day + m * 60));
    CHECK(hopBand(all, day + 119) == 0 && hopBand(all, day + 120) == 1 && hopNextChange(all, day + 119) == day + 120 && hopNextChange(all, day + 120) == day + 240, "slot edges");
    HopPlan no60 = HopPlan::allHours(0x3FF & ~(1 << 2));
    CHECK(hopBand(no60, day + 4 * 60) == 3, "60 m off: 40 m in its slot");
    HopPlan six = HopPlan::allHours(1 << 10);
    CHECK(hopBand(six, day) == 10 && hopBand(six, day + 600) == 10, "only 6 m (outside the table)");
    HopPlan rot = HopPlan::allHours((1 << 3) | (1 << 5) | (1 << 9));
    rot.coordinated = false;
    CHECK(hopBand(rot, day) == 3 && hopBand(rot, day + 120) == 5 && hopBand(rot, day + 240) == 9 && hopBand(rot, day + 360) == 3, "rotation 40 20 10 m");
    CHECK(hopBand(rot, day - 120) == 9, "before 1970 / negative slot: still in turn");
    HopPlan hrs;
    hrs.hours[0] = 1 << 1;   // 00 UTC: 80 m only
    hrs.hours[12] = (1 << 5) | (1 << 7);   // 12 UTC: 20 and 15 m
    CHECK(hopBand(hrs, day + 600) == 1 && hopBand(hrs, day + 3600) == -1 && hopBand(hrs, day + 12 * 3600 + 10 * 60) == 5 && hopBand(hrs, day + 12 * 3600 + 14 * 60) == 7 &&
          hopBand(hrs, day + 12 * 3600 + 16 * 60) == 5, "per hour, nothing on at 01 UTC");
    const HopPlan back = hopFromText(hopToText(hrs));
    CHECK(back.coordinated && back.hours[0] == 2 && back.hours[12] == 0xA0 && back.hours[5] == 0, "settings round trip: %s", hopToText(hrs).c_str());
    CHECK(!hopFromText("c=0").coordinated && hopFromText("junk").hours[3] == 0, "bad text");
    return testSummary("hfdig_hop");
}
