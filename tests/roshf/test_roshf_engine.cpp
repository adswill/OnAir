// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ROS (HF) through the engine (setStandard(207)): the synthetic source plays the reported-grid test signal, the receiver reports through latestRx().
#include "dect2/roshf/roshf_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = roshfTuning();
    CHECK(mt.stdMode == 207 && modeTuningById("roshf") && modeTuningById("roshf")->stdMode == 207 && modeTuning(207), "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = 1e6;
    t.synth.mode = 207;
    t.synth.snrDb = 15;
    FileOptions fo;
    e.setStandard(207);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 206, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 26.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 206) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const RoshfTelemetry& s = rx.roshf;
    printf("  %d reports: %s\n", reports, roshfSummary(s).c_str());
    CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(s.state == 2 && s.subId == "ros16", "state %d sub-mode %s", s.state, s.subId.c_str());
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return testSummary("roshf engine");
}
