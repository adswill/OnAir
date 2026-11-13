// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AIS: the decoded lists live through reset() and configure(), go into the results file and come back from it, and only
// clearResults() empties them (tests/results/results_testkit.h).
#include "../results/results_testkit.h"
#include "dect2/ais/ais_rx.h"
#include "dect2/acars/acars_rx.h"
#include "dect2/source/source.h"
#include <cstdio>
using namespace dect2;

int main() {
    SynthConfig cfg;
    cfg.mode = 16;
    cfg.snrDb = 30;
    rtk::checkReceiver<AisReceiver, AisTelemetry>("AIS", 2e6, makeModeSynth(16, cfg, 2e6), 12,
        [](const AisTelemetry& t) { return (size_t)t.vessels.size(); }, AcarsReceiver().saveResults());
    printf(rtk::fails ? "AIS results: %d FAILED\n" : "AIS results: all passed\n", rtk::fails);
    return rtk::fails ? 1 : 0;
}
