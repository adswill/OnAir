// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// M17 through the engine (setStandard(33)): the synthetic source plays the test schedule, the receiver reports through latestRx().
#include "dect2/m17/m17_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = m17Tuning();
    CHECK(mt.stdMode == 33 && modeTuningById("m17") && modeTuningById("m17")->stdMode == 33, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 33;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.m17().setSilent(true);
    e.setStandard(33);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 32, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 12.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 32) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const M17Telemetry& m = rx.m17;
    printf("  %d reports: %s, %zu transmissions, %zu messages, %zu positions, %llu LSF\n", reports, m17Summary(m).c_str(), m.log.size(), m.messages.size(), m.positions.size(),
           (unsigned long long)m.lsfOk);
    CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(m.lsfOk >= 3 && m.log.size() >= 3, "%llu LSF, %zu transmissions", (unsigned long long)m.lsfOk, m.log.size());
    CHECK(!m.messages.empty() && !m.positions.empty(), "messages and positions");
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    printf(fails ? "m17 engine: %d FAILED\n" : "m17 engine: all passed\n", fails);
    return fails ? 1 : 0;
}
