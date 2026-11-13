// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sub-GHz sensors through the engine (setStandard(243)): the synthetic source plays Nexus-type sensors and one unknown PWM burst, the receiver reports through latestRx().
#include "dect2/sensors/sensors_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = sensorsTuning();
    CHECK(mt.stdMode == 243 && modeTuning(243), "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 243;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.setStandard(243);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 242, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 25.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 242) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const SensorsTelemetry& s = rx.sensors;
    printf("  %d reports: %s\n", reports, sensorsSummary(s).c_str());
    CHECK(reports > 3 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    for (const auto& d : s.devices) printf("    %s id %s ch %s x%llu\n", d.model.c_str(), d.id.c_str(), d.channel.c_str(), (unsigned long long)d.count);
    CHECK(s.devices.size() >= 3, "three Nexus sensors, got %zu", s.devices.size());
    CHECK(!s.unknown.empty(), "the unknown burst is analysed");
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return testSummary("sensors engine");
}
