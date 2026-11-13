// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The BLE tracker detector through the engine (setStandard(145)): the synthetic source plays the trackers and the receiver reports through latestRx().
#include "dect2/engine/engine.h"
#include "dect2/bletrk/bletrk_live.h"
#include "dect2/bletrk/bletrk_tel.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = bletrkTuning();
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6; t.bandwidthMhz = mt.bandwidthMhz; t.sampleRate = mt.sampleRate;
    t.synth.mode = 145; t.synth.snrDb = 30;
    FileOptions fo;
    e.setStandard(145);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 144, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    bool got = false;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 40) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; if (rx.standard == 144 && rx.bletrk.trackers.size() >= 5) { got = true; break; } }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    printf("  %s\n", bletrkSummary(rx.bletrk).c_str());
    CHECK(got, "fewer than 5 trackers in 40 s");
    CHECK(rx.rateOk && rx.dataValid, "rate %d valid %d", (int)rx.rateOk, (int)rx.dataValid);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return testSummary("bletrk engine");
}
