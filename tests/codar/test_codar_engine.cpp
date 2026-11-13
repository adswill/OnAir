// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CODAR / HF ocean radar through the engine (setStandard(74)): the synthetic source plays the test signal, the receiver reports through latestRx().
// No sound.
#include "dect2/codar/codar_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = codarTuning();
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = 13.0e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 74;
    t.synth.snrDb = 22;
    FileOptions fo;
    e.setStandard(74);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 73, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 9.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 73) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const CodarTelemetry& d = rx.codar;
    printf("  %d reports: %s, %llu sweeps\n", reports, codarSummary(d).c_str(), (unsigned long long)d.blocksOk);
    CHECK(reports > 10 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(d.stations.size() == 3 && d.blocksOk > 12, "%zu radars, %llu sweeps", d.stations.size(), (unsigned long long)d.blocksOk);
    CHECK(d.state == 1 && rx.dataValid, "state");
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.codar().clearResults();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    e.stop();
    printf(fails ? "codar engine: %d FAILED\n" : "codar engine: all passed\n", fails);
    return fails ? 1 : 0;
}
