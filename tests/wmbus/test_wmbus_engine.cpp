// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wireless M-Bus through the engine (setStandard(88)): the synthetic source plays the 868 MHz test signal and the receiver reports through latestRx().
#include "dect2/engine/engine.h"
#include "dect2/wmbus/wmbus_live.h"
#include "dect2/wmbus/wmbus_tel.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = wmbusTuning();
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6; t.bandwidthMhz = mt.bandwidthMhz; t.sampleRate = mt.sampleRate;
    t.synth.mode = 88; t.synth.snrDb = 30;
    FileOptions fo;
    e.setStandard(88);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 87, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    bool got = false;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 40) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; if (rx.standard == 87 && rx.wmbus.devices.size() >= 5) { got = true; break; } }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    printf("  %s, %zu channels\n", wmbusSummary(rx.wmbus).c_str(), rx.wmbus.channels.size());
    CHECK(got, "fewer than 5 meters in 40 s");
    CHECK(rx.rateOk && rx.dataValid, "rate %d valid %d", (int)rx.rateOk, (int)rx.dataValid);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return testSummary("wmbus engine");
}
