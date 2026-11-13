// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Datawell buoy HF link through the engine (setStandard(76)): the synthetic source plays the 2FSK HXV test signal, the receiver reports through
// latestRx(). No sound.
#include "dect2/datawell/datawell_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = datawellTuning();
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = 30.475e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 76;
    t.synth.snrDb = 24;
    FileOptions fo;
    e.setStandard(76);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 75, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 40.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 75) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const DatawellTelemetry& d = rx.datawell;
    printf("  %d reports: %s, %llu vectors\n", reports, datawellSummary(d).c_str(), (unsigned long long)d.hxvVectors);
    CHECK(reports > 10 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(d.state == 2 && d.format == "2FSK" && d.hxvLocked, "state %d format '%s' lock %d", d.state, d.format.c_str(), d.hxvLocked);
    CHECK(d.hxvVectors >= 30 && d.hxvLost <= 3 && rx.dataValid, "vectors %llu lost %llu", (unsigned long long)d.hxvVectors, (unsigned long long)d.hxvLost);
    CHECK(d.channel == "Default", "channel '%s' at %.4f MHz", d.channel.c_str(), d.freqMhz);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.datawell().clearResults();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    e.stop();
    printf(fails ? "datawell engine: %d FAILED\n" : "datawell engine: all passed\n", fails);
    return fails ? 1 : 0;
}
