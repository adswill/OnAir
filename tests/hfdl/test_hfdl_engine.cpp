// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HFDL through the engine (setStandard(37)): the synthetic source plays the test schedule, the receiver reports through latestRx().
#include "dect2/hfdl/hfdl_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = hfdlTuning();
    CHECK(mt.stdMode == 37 && modeTuningById("hfdl") && modeTuningById("hfdl")->stdMode == 37, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 37;
    t.synth.modeVal[0] = 3;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.setStandard(37);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 36, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 40.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 36) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const HfdlTelemetry& m = rx.hfdl;
    printf("  %d reports: %s, %zu messages, %llu squitters, %llu ACARS, %zu aircraft\n", reports, hfdlSummary(m).c_str(), m.messages.size(), (unsigned long long)m.squitters,
           (unsigned long long)m.acarsMsgs, m.aircraft.size());
    CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(m.squitters >= 3 && m.acarsMsgs >= 2 && m.blocksBad == 0, "%llu squitters, %llu ACARS, %llu bad", (unsigned long long)m.squitters, (unsigned long long)m.acarsMsgs, (unsigned long long)m.blocksBad);
    int inBand = 0; for (const auto& c : m.channels) inBand += c.inBand;
    CHECK(inBand >= 3, "%d channels in the band", inBand);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    printf(fails ? "hfdl engine: %d FAILED\n" : "hfdl engine: all passed\n", fails);
    return fails ? 1 : 0;
}
