// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// D-STAR through the engine (setStandard(35)): the synthetic source plays the test schedule, the receiver reports through latestRx().
// Silent: the receiver never opens the sound device.
#include "dect2/dstar/dstar_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = dstarTuning();
    CHECK(mt.stdMode == 35 && modeTuningById("dstar") && modeTuningById("dstar")->stdMode == 35, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 35;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.dstar().setSilent(true);
    e.setStandard(35);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 34, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 12.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 34) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const DstarTelemetry& d = rx.dstar;
    printf("  %d reports: %s, %zu calls, %zu messages, %zu positions, %llu headers\n", reports, dstarSummary(d).c_str(), d.log.size(), d.messages.size(), d.positions.size(),
           (unsigned long long)d.headersOk);
    CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(d.headersOk >= 3 && d.log.size() >= 2, "%llu headers, %zu calls", (unsigned long long)d.headersOk, d.log.size());
    CHECK(!d.messages.empty() && d.positions.size() >= 2, "messages and positions");
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    printf(fails ? "dstar engine: %d FAILED\n" : "dstar engine: all passed\n", fails);
    return fails ? 1 : 0;
}
