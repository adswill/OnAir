// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Robust Packet through the engine (setStandard(901)): the synthetic source plays the test signal, the receiver reports through latestRx().
#include "dect2/rpr/rpr_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"
#include "../common/test_long.h"

int main() {
    const ModeTuning mt = rprTuning();
    CHECK(mt.stdMode == 901 && modeTuning(901) && modeTuningById("rpr") && modeTuningById("rpr")->stdMode == 901, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = 250000;
    t.synth.mode = 901;
    t.synth.snrDb = 20;
    t.synth.modeOpt[0] = 2;
    FileOptions fo;
    e.setStandard(901);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 900, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    auto passed = [&] { const RprTelemetry& q = rx.rpr; return reports > 10 && wrong == 0 && rx.rateOk && q.level == 2 && q.blocksOk >= 2 && e.droppedSamples() == 0; };
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 45.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 900) wrong++; }
        if (!testLong() && passed()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const RprTelemetry& p = rx.rpr;
    printf("  %d reports: %s\n", reports, rprSummary(p).c_str());
    CHECK(reports > 10 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(p.level == 2 && p.state == 2, "level %d state %d", p.level, p.state);
    CHECK(p.blocksOk >= 2 && std::fabs(p.centreHz - 1500) < 4, "%llu bursts, centre %.1f", (unsigned long long)p.blocksOk, p.centreHz);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return testSummary("rpr engine");
}
