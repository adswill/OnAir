// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// dPMR through the engine (setStandard(47)): the synthetic source plays the test schedule, the receiver reports through latestRx().
// Silent: the receiver never opens the sound device.
#include "dect2/dpmr/dpmr_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = dpmrTuning();
    CHECK(mt.stdMode == 47 && modeTuningById("dpmr") && modeTuningById("dpmr")->stdMode == 47, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 47;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.dpmr().setSilent(true);
    e.setStandard(47);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 46, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 12.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 46) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const DpmrTelemetry& d = rx.dpmr;
    printf("  %d reports: %s, %zu log entries, %llu frames\n", reports, dpmrSummary(d).c_str(), d.log.size(), (unsigned long long)d.frames);
    CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(d.log.size() >= 4, "%zu log entries", d.log.size());
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    printf(fails ? "dpmr engine: %d FAILED\n" : "dpmr engine: all passed\n", fails);
    return fails ? 1 : 0;
}
