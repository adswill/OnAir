// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The QO-100 narrowband band plan: gap-free coverage, segment lookup, width check, level over the beacon, the plain status.
#include "dect2/qo100/qo100_plan.h"
#include "dect2/qo100/qo100_dsp.h"
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
    return testSummary("qo100_plan");
}
