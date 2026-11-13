// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ANT+ through the engine (setStandard(79)): the synthetic source plays the test sensors, the receiver reports through latestRx().
#include "dect2/engine/engine.h"
#include "dect2/ant/ant_rx.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = antTuning();
    CHECK(mt.stdMode == 79 && modeTuningById("ant") && modeTuningById("ant")->stdMode == 79, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = 2457e6 + mt.tuneOffsetHz;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 79;
    t.synth.snrDb = 25;
    t.synth.modeOpt[0] = 0;
    FileOptions fo;
    e.setStandard(79);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 78, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 8.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 78) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const AntTelemetry& d = rx.ant;
    printf("  %d reports: %s\n", reports, antSummary(d).c_str());
    CHECK(reports > 10 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(d.devices.size() == 3, "%zu sensors", d.devices.size());
    bool hr = false;
    for (const auto& x : d.devices) if (x.type == ant::HeartRate && x.heartRate > 0) hr = true;
    CHECK(hr, "no heart rate");
    CHECK(d.blocksOk >= 20, "%llu frames", (unsigned long long)d.blocksOk);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.ant().clearResults();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    e.stop();
    printf(fails ? "ant engine: %d FAILED\n" : "ant engine: all passed\n", fails);
    return fails ? 1 : 0;
}
