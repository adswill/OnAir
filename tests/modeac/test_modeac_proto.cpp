// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode A / C reply code: squawk digits, the Gillham altitude (checked against the Mode S decoder's own Gillham table and for one bit per step).
#include "dect2/modeac/modeac_proto.h"
#include "dect2/adsb/adsb_msg.h"
#include <cstdio>
using namespace dect2;
using namespace dect2::modeac;
#include "../common/test_util.h"

int main() {
    // squawk: all 4096 codes round-trip
    int bad = 0;
    for (int a = 0; a < 8; a++) for (int b = 0; b < 8; b++) for (int c = 0; c < 8; c++) for (int d = 0; d < 8; d++) {
        const int sq = a * 1000 + b * 100 + c * 10 + d;
        if (squawkOf(bitsOfSquawk(sq)) != sq) bad++;
    }
    CHECK(bad == 0, "%d squawks do not round-trip", bad);
    // 7700: A4 A2 A1 and B4 B2 B1 set, nothing else
    const uint16_t e = bitsOfSquawk(7700);
    CHECK(slotBit(e, 2) && slotBit(e, 4) && slotBit(e, 6) && slotBit(e, 8) && slotBit(e, 10) && slotBit(e, 12), "7700 pulses");
    CHECK(!slotBit(e, 1) && !slotBit(e, 3) && !slotBit(e, 5) && !slotBit(e, 7) && !slotBit(e, 9) && !slotBit(e, 11) && !slotBit(e, 13), "7700 has no other pulses");
    CHECK(squawkOf(bitsOfSquawk(1200)) == 1200 && slotBit(bitsOfSquawk(1200), 2) && slotBit(bitsOfSquawk(1200), 10), "1200: A1 and B2");
    CHECK(isEmergencySquawk(7500) && isEmergencySquawk(7600) && isEmergencySquawk(7700) && !isEmergencySquawk(1200), "emergency codes");
    CHECK(std::string(slotName(1)) == "C1" && std::string(slotName(13)) == "D4" && std::string(slotName(7)) == "X", "slot names");
    CHECK(squawkText(7) == "0007" && squawkText(7700) == "7700", "squawk text");

    // altitude: every 100 ft step from -1200 to 126700 round-trips, one pulse changes per step, agrees with the Mode S table
    int rt = 0, jump = 0, xs = 0, cross = 0, crossChecked = 0;
    uint16_t prev = 0;
    for (int ft = -1200; ft <= 126700; ft += 100) {
        const uint16_t b = bitsOfAltitude(ft);
        const Altitude a = altitudeOf(b);
        if (!a.valid || a.feet != ft) rt++;
        if (slotBit(b, 7)) xs++;
        if (ft > -1200) {
            const uint16_t x = (uint16_t)(b ^ prev);
            if (__builtin_popcount(x) != 1) jump++;
        }
        prev = b;
        if (ft <= 62700) {   // the Mode S table has no D1
            unsigned code11 = 0;
            const int order[11] = {1, 2, 3, 4, 5, 6, 8, 10, 11, 12, 13};   // C1 A1 C2 A2 C4 A4 B1 B2 D2 B4 D4
            for (int s : order) code11 = (code11 << 1) | (unsigned)slotBit(b, s);
            int alt = 0;
            crossChecked++;
            if (!adsb::gillhamToAltitude(code11, alt) || alt != ft) cross++;
        }
    }
    CHECK(rt == 0, "%d altitudes do not round-trip", rt);
    CHECK(jump == 0, "%d steps change more than one pulse", jump);
    CHECK(xs == 0, "X is set in %d altitude codes", xs);
    CHECK(cross == 0, "%d of %d codes differ from the Mode S Gillham table", cross, crossChecked);
    CHECK(altitudeOf(bitsOfAltitude(35000)).feet == 35000 && altitudeOf(bitsOfAltitude(0)).feet == 0, "35000 and 0 ft");
    CHECK(altitudeOf(bitsOfAltitude(-5000)).feet == -1200 && altitudeOf(bitsOfAltitude(200000)).feet == 126700, "clamped");
    // patterns that are not altitudes
    CHECK(!altitudeOf(0).valid, "no pulses");
    CHECK(!altitudeOf(bitsOfSquawk(7700)).valid, "7700: C bits 000 are not a step");
    CHECK(!altitudeOf(bitsOfSquawk(1277)).valid, "C bits 111");
    uint16_t x = bitsOfAltitude(10000); x = setSlot(x, 7, true);
    CHECK(!altitudeOf(x).valid, "X set");
    return testSummary("modeac proto");
}
