// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// P25 through the engine (setStandard(52)): the synthetic source plays the test schedule, the receiver reports through latestRx().
// Silent: the receiver never opens the sound device.
#include "dect2/p25/p25_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = p25Tuning();
    CHECK(mt.stdMode == 52 && modeTuningById("p25") && modeTuningById("p25")->stdMode == 52, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 52;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.p25().setSilent(true);
    e.setStandard(52);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 51, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 12.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 51) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const P25Telemetry& d = rx.p25;
    printf("  %d reports: %s, %zu log entries, %llu frames, site %s\n", reports, p25Summary(d).c_str(), d.log.size(), (unsigned long long)d.frames, d.site.valid ? "yes" : "no");
    CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(d.log.size() >= 4 && d.site.valid, "%zu log entries", d.log.size());
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    printf(fails ? "p25 engine: %d FAILED\n" : "p25 engine: all passed\n", fails);
    return fails ? 1 : 0;
}
