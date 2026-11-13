// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRA SDS interpreter: D-SDS-DATA, SDS-TL protocol identifier, LIP short location report and simple text, by round trip with the generator, over the
// whole globe, every code of position error / speed / heading, and truncated or random bits (never a crash, never a false position).
#include "dect2/tetra/tetra_sds.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2::tetra;
#include "../common/test_util.h"

int main() {
    std::mt19937 g(9);
    // positions: error under the quantisation step (1.1e-5 degrees)
    double worst = 0; int n = 0;
    for (int i = 0; i < 2000; i++) {
        const double lat = -89.9 + 179.8 * (g() % 100000) / 100000.0, lon = -179.9 + 359.8 * (g() % 100000) / 100000.0;
        const int err = (int)(g() % 8), spd = (int)(g() % 128), dir = (int)(g() % 16), tel = (int)(g() % 4), reason = (int)(g() % 256);
        const Bits pdu = buildSdsData(2000000 + (int)(g() % 1000), buildLipShort(lat, lon, err, spd, dir, tel, reason), 0x0A);
        SdsMsg m;
        // buildSdsData prefixes the PID: the user data is the LIP PDU
        CHECK(parseSdsData(pdu, m) && m.valid && m.kind == "location" && m.hasPos, "parsed %d", i);
        worst = std::max(worst, std::max(std::fabs(m.lat - lat), std::fabs(m.lon - lon)));
        CHECK(m.timeElapsed == tel && m.reason == reason, "fields");
        CHECK(m.headingDeg == (dir == 15 ? -1 : (int)std::lround(dir * 22.5)), "heading");
        CHECK(spd == 127 ? m.speedKmh < 0 : m.speedKmh >= 0, "speed");
        n++;
    }
    printf("LIP round trip over %d random positions: worst error %.2e degrees (step 1.1e-5)\n", n, worst);
    CHECK(worst < 1.2e-5, "quantisation");
    // text
    {
        SdsMsg m;
        CHECK(parseSdsData(buildSdsText(2001234, "Unit 7 on scene, send EMS"), m) && m.kind == "text" && m.text == "Unit 7 on scene, send EMS" && m.callingIssi == 2001234, "text");
    }
    // not an SDS, cut short, random bits: no crash, no false position
    {
        SdsMsg m;
        Bits other(100, 0);
        CHECK(!parseSdsData(other, m), "type 0 is not D-SDS-DATA");
        const Bits ok = buildSdsData(5, buildLipShort(10, 20, 1, 2, 3, 0, 0), 0x0A);
        for (size_t cut = 0; cut < ok.size(); cut++) { Bits c(ok.begin(), ok.begin() + (long)cut); SdsMsg x; if (parseSdsData(c, x)) CHECK(!x.hasPos || cut >= ok.size() - 0, "cut %zu", cut); }
        int pos = 0;
        for (int i = 0; i < 20000; i++) { Bits r(120); for (auto& b : r) b = g() & 1; r[0] = 0; r[1] = 1; r[2] = 1; r[3] = 1; r[4] = 1; SdsMsg x; if (parseSdsData(r, x) && x.hasPos) pos++; }
        printf("random D-SDS-DATA bits with PID 0x0A-like content giving a position: %d of 20000\n", pos);
        CHECK(pos < 20, "random bits rarely look like a position (%d)", pos);
    }
    return testSummary("tetra sds");
}
