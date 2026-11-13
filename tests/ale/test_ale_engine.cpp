// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF ALE through the engine (setStandard(51)): the synthetic source plays the test calls, the receiver reports through latestRx().
#include "dect2/ale/ale_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = aleTuning();
    CHECK(mt.stdMode == 51 && modeTuningById("ale") && modeTuningById("ale")->stdMode == 51, "tuning table");
    Engine e;
    e.ale().setSilent(true);
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 51;
    FileOptions fo;
    e.setStandard(51);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 50, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 12.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 50) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const AleTelemetry& a = rx.ale;
    bool sounding = false, call = false;
    for (const auto& v : a.events) {
        printf("  %-12s %s -> %s %s\n", v.type.c_str(), v.from.c_str(), v.to.c_str(), v.amd.c_str());
        if (v.type == "Sounding" && v.from == "LY2ABC" && std::fabs(v.dialHz - mt.defMhz * 1e6) < 1) sounding = true;
        if (v.type == "Individual" && v.from == "LY2ABC" && v.to == "OH2XYZ") call = true;
    }
    printf("  %d reports: %s\n", reports, aleSummary(a).c_str());
    CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(sounding && call, "sounding %d, call %d", sounding, call);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    printf(fails ? "ale engine: %d FAILED\n" : "ale engine: all passed\n", fails);
    return fails ? 1 : 0;
}
