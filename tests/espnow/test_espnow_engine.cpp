// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ESP-NOW through the engine (setStandard(900)): the synthetic source plays the test scene, the receiver reports through latestRx(). No sound.
#include "dect2/espnow/espnow_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = espnowTuning();
    CHECK(mt.stdMode == 900 && modeTuningById("espnow") && modeTuningById("espnow")->stdMode == 900, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 900;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.setStandard(900);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 899, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 20.0 && !(reports > 4 && rx.espnow.devices.size() == 5 && rx.espnow.espnowFrames > 60)) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 899) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const EspnowTelemetry& d = rx.espnow;
    printf("  %d reports: %s, %llu frames, %llu bad, channel %d\n", reports, espnowSummary(d).c_str(), (unsigned long long)d.espnowFrames, (unsigned long long)d.blocksBad, d.channel);
    for (const auto& x : d.devices) printf("  %s %s %llu packets, %.1f/s\n", x.mac.c_str(), x.vendor.c_str(), (unsigned long long)x.packets, x.rate);
    CHECK(reports > 4 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(d.devices.size() == 5 && d.espnowFrames > 60, "%zu devices, %llu frames", d.devices.size(), (unsigned long long)d.espnowFrames);
    CHECK(d.channel == 1, "channel %d", d.channel);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return testSummary("espnow engine");
}
