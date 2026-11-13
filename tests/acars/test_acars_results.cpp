// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ACARS: the decoded lists live through reset() and configure(), go into the results file and come back from it, and only
// clearResults() empties them (tests/results/results_testkit.h).
#include "../results/results_testkit.h"
#include "dect2/acars/acars_rx.h"
#include "dect2/ais/ais_rx.h"
#include "dect2/source/source.h"
#include <cstdio>
using namespace dect2;

int main() {
    SynthConfig cfg;
    cfg.mode = 18;
    cfg.snrDb = 30;
    cfg.modeVal[0] = 3;                     // a block every 5 s per aircraft
    rtk::checkReceiver<AcarsReceiver, AcarsTelemetry>("ACARS", 2e6, makeModeSynth(18, cfg, 2e6), 16,
        [](const AcarsTelemetry& t) { return (size_t)t.messages.size(); }, AisReceiver().saveResults());
    {   // the results file format: its own types only, and nothing changes when it is refused
        std::vector<AcarsMessage> m(2);
        m[1].text = "persist";
        const std::string b = results::pack(m);
        std::vector<AcarsMessage> back;
        RCHECK(results::unpack(b, back) && back.size() == 2 && back[1].text == "persist", "round trip of a message list");
        std::vector<AisVessel> other(1);
        RCHECK(!results::unpack(b, other) && other.size() == 1, "a message list read as vessels");
        RCHECK(results::signature<AcarsMessage>() != results::signature<AisVessel>(), "two structs with one signature");
    }
    printf(rtk::fails ? "ACARS results: %d FAILED\n" : "ACARS results: all passed\n", rtk::fails);
    return rtk::fails ? 1 : 0;
}
