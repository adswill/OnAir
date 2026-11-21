// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The QO-100 narrowband band plan: gap-free coverage, segment lookup, width check, level over the beacon, the plain status.
#include "dect2/qo100/qo100_plan.h"
#include "dect2/qo100/qo100_dsp.h"
#include "dect2/qo100/qo100_rx.h"
#include <cmath>
#include <vector>
#include <cstdio>
#include <cstring>
using namespace dect2;
#include "../common/test_util.h"
int main() {
    int n = 0; const qo100::PlanSegment* p = qo100::bandPlan(n);
    CHECK(n == 9, "segments %d", n);
    CHECK(p[0].loHz == qo100::kNbLoHz && p[n - 1].hiHz == qo100::kNbHiHz, "plan spans the transponder");
    for (int i = 1; i < n; i++) CHECK(p[i].loHz == p[i - 1].hiHz, "gap before segment %d", i);
    CHECK(qo100::segmentAt(10489.700e6)->use == qo100::kPlanSsb, "700 kHz is SSB");
    CHECK(qo100::segmentAt(10489.750e6)->use == qo100::kPlanBeacon, "PSK beacon");
    CHECK(qo100::segmentAt(10489.510e6)->use == qo100::kPlanCw, "CW segment");
    CHECK(qo100::segmentAt(10489.560e6)->use == qo100::kPlanNarrowDigital, "narrow digital");
    CHECK(qo100::segmentAt(10489.600e6)->use == qo100::kPlanDigital, "digital");
    CHECK(qo100::segmentAt(10489.900e6)->use == qo100::kPlanMixed, "mixed");
    CHECK(!qo100::segmentAt(10490.500e6) && !qo100::segmentAt(10489.4e6), "outside");
    CHECK(qo100::planProblem(10489.700e6, 2400).empty(), "SSB of 2.4 kHz fits");
    CHECK(!qo100::planProblem(10489.700e6, 4000).empty(), "4 kHz SSB too wide");
    CHECK(!qo100::planProblem(10489.510e6, 2400).empty(), "SSB in the CW segment");
    CHECK(qo100::planProblem(10489.510e6, 150).empty(), "CW fits");
    CHECK(!qo100::planProblem(10491e6, 100).empty(), "outside the transponder");

    Qo100Telemetry t;
    double rel = 0;
    CHECK(!qo100::levelOverBeaconDb(t, -80, rel), "no beacon, no reference");
    Qo100Signal b; b.beacon = 1; b.levelDb = -70; t.signals.push_back(b);
    CHECK(qo100::levelOverBeaconDb(t, -85, rel) && rel == -15, "15 dB under the beacon (%.1f)", rel);

    CHECK(qo100::plainStatus(t, false).find("Start") == 0, "stopped");
    t.rate = 2e6; t.state = 1;
    CHECK(qo100::plainStatus(t, true).find("Looking") == 0, "searching");
    t.state = 2; t.corrValid = true; t.lnbErrHz = -3200;
    const std::string s = qo100::plainStatus(t, true);
    CHECK(s.find("3.2 kHz low") != std::string::npos, "status: %s", s.c_str());
    // a click picks mode and dial: CW 700 Hz under the carrier, SSB / digital USB with the dial 300 Hz under the lower edge
    {
        auto c = qo100::clickTune(10489.512e6, 100);
        CHECK(std::strcmp(c.mode, "CW") == 0 && std::fabs(c.dialHz - (10489.512e6 - 700)) < 1e-3, "CW click: %s %.1f", c.mode, c.dialHz);
        auto v = qo100::clickTune(10489.700e6, 2400);
        CHECK(std::strcmp(v.mode, "USB (SSB)") == 0 && std::fabs(v.dialHz - (10489.700e6 - 2700)) < 1e-3, "SSB click: %s %.1f", v.mode, v.dialHz);
        auto d = qo100::clickTune(10489.640e6, 2000);
        CHECK(std::strcmp(d.mode, "USB (digital)") == 0, "digital click: %s", d.mode);
        auto n = qo100::clickTune(10489.560e6, 50);
        CHECK(std::strcmp(n.mode, "CW") == 0, "narrow carrier in the narrow digital segment is a beat note");
    }
    // DATV quality words
    CHECK(qo100::datvQuality(0.0, false, false) == qo100::kDatvNone, "none");
    CHECK(qo100::datvQuality(2.5, false, false) == qo100::kDatvWeak, "weak");
    CHECK(qo100::datvQuality(6.0, false, false) == qo100::kDatvUsable, "usable");
    CHECK(qo100::datvQuality(12.0, false, false) == qo100::kDatvGood, "good");
    CHECK(qo100::datvQuality(5.0, true, true) == qo100::kDatvLocked, "locked");
    CHECK(qo100::datvQuality(10.0, true, true) == qo100::kDatvGood, "locked and good");
    CHECK(qo100::datvQuality(6.0, true, false) == qo100::kDatvUsable, "decoding without a stream is not locked");
    CHECK(std::fabs(qo100::datvMarginDb(7.0) - 5.0) < 1e-9, "margin");
    // the per-LNB memory
    {
        qo100::LnbMemory m;
        double ppm = 0;
        CHECK(!m.recall(9750e6, ppm), "nothing remembered");
        m.remember(9750e6, 3.214); m.remember(10600e6, -1.5);
        CHECK(m.recall(9750e6, ppm) && std::fabs(ppm - 3.214) < 1e-9, "recall 9750");
        qo100::LnbMemory m2; m2.parse(m.serialize());
        CHECK(m2.size() == 2 && m2.recall(10600e6, ppm) && std::fabs(ppm + 1.5) < 1e-3, "round trip");
        qo100::LnbMemory m3; m3.parse("junk;9750000:abc;:5;10000000:2.5;8000000:9999");
        CHECK(m3.size() == 1 && m3.recall(10000e6, ppm) && ppm == 2.5, "bad entries skipped (%zu)", m3.size());
        Qo100Telemetry t; t.rate = 2e6; t.state = 1; t.seeded = true; t.seedErrHz = -3200;
        CHECK(qo100::plainStatus(t, true).find("3.2 kHz LNB error remembered") != std::string::npos, "seeded status");
    }
    // the receiver reports the remembered error while it has not locked (noise only)
    for (int withSeed = 0; withSeed < 2; withSeed++) {
        Qo100Receiver rx;
        rx.configure(2e6);
        rx.setLo(9750e6);
        rx.setCenterHz(qo100::ifOf(qo100::kNarrowCentreHz, 9750e6));
        if (withSeed) rx.setSeedErrorHz(27000);
        std::vector<cf32> buf(20000);
        uint32_t st = 12345;
        auto rnd = [&st]() { st = st * 1664525u + 1013904223u; return (float)((st >> 8) & 0xFFFF) / 65536.f - 0.5f; };
        uint64_t seq = 0; Qo100Telemetry last; bool got = false;
        for (int b = 0; b < 30; b++) {
            for (auto& v : buf) v = cf32(rnd() * 0.01f, rnd() * 0.01f);
            rx.feed(buf.data(), buf.size());
            Qo100Telemetry t;
            if (rx.telemetry(t, seq)) { seq = t.seq; last = std::move(t); got = true; }
        }
        CHECK(got, "no telemetry");
        CHECK(last.seeded == (withSeed == 1) && (!withSeed || last.seedErrHz == 27000), "seed %d: seeded %d err %.0f", withSeed, (int)last.seeded, last.seedErrHz);
        CHECK(!last.corrValid, "noise does not lock");
    }
    return testSummary("qo100_plan");
}
