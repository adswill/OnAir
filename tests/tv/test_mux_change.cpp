// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// isDifferentMux(): when the live service list must be emptied; and the demux dropping the services of the old transport stream.
#include "dect2/tv/mux_change.h"
#include "dect2/tv/ts.h"
#include "dect2/tv/ts_proto.h"
#include <cstdio>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    MuxId a; a.freqHz = 11.7e9 - 9.75e9; a.symRateHz = 27.5e6; a.family = 5; a.onid = 1; a.tsid = 10;
    MuxId b = a;
    CHECK(!isDifferentMux(a, b), "identical");
    b.freqHz += 20e3; CHECK(!isDifferentMux(a, b), "small drift is the same mux");
    b.freqHz = a.freqHz + 12e6; CHECK(isDifferentMux(a, b), "other transponder");
    b = a; b.symRateHz = 22e6; CHECK(isDifferentMux(a, b), "other symbol rate");
    b = a; b.symRateHz = 0; CHECK(!isDifferentMux(a, b), "unknown symbol rate is no change");
    b = a; b.family = 6; CHECK(isDifferentMux(a, b), "other family");
    b = a; b.tsid = 11; CHECK(isDifferentMux(a, b), "other ts id");
    b = a; b.onid = 2; CHECK(isDifferentMux(a, b), "other network id");
    b = a; b.tsid = 0; b.onid = 0; CHECK(!isDifferentMux(a, b), "ids not heard yet");
    MuxId z; z.freqHz = a.freqHz; z.family = a.family; CHECK(!isDifferentMux(z, a), "first ids heard after a retune");

    TsDemux d;
    uint8_t p[188];
    tsp::patPacket(1, 100, 0x100, 0, p); d.feed(p);
    CHECK(d.snapshot().services.size() == 1 && d.snapshot().tsid == 1, "ts 1 listed (%zu)", d.snapshot().services.size());
    tsp::patPacket(2, 200, 0x200, 0, p); d.feed(p);
    TsSnapshot s = d.snapshot();
    CHECK(s.tsid == 2 && s.services.size() == 1 && s.services[0].id == 200, "ts 2 replaces the list (%zu services, tsid %d)", s.services.size(), s.tsid);
    if (fails == 0) printf("mux_change OK\n");
    return fails ? 1 : 0;
}
