// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ADS-B: the decoded lists live through reset() and configure(), go into the results file and come back from it, and only
// clearResults() empties them (tests/results/results_testkit.h).
#include "../results/results_testkit.h"
#include "dect2/adsb/adsb_rx.h"
#include "dect2/ais/ais_rx.h"
#include "dect2/source/source.h"
#include <cstdio>
using namespace dect2;

int main() {
    SynthConfig cfg;
    cfg.mode = 13;
    cfg.snrDb = 30;
    cfg.modeOpt[0] = 12;
    rtk::checkReceiver<AdsbReceiver, AdsbTelemetry>("ADS-B", 4e6, makeModeSynth(13, cfg, 4e6), 6,
        [](const AdsbTelemetry& t) { return (size_t)t.aircraft.size(); }, AisReceiver().saveResults());
    printf(rtk::fails ? "ADS-B results: %d FAILED\n" : "ADS-B results: all passed\n", rtk::fails);
    return rtk::fails ? 1 : 0;
}
