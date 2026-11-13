// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MT63 through the engine (setStandard(97)): the synthetic source plays the test signal, the receiver reports through latestRx().
#include "dect2/mt63/mt63_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = mt63Tuning();
    CHECK(mt.stdMode == 97 && modeTuningById("mt63") && modeTuningById("mt63")->stdMode == 97, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 97;
    t.synth.snrDb = 8;
    FileOptions fo;
    e.setStandard(97);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 96, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 40.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 96) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const Mt63Telemetry& p = rx.mt63;
    printf("  %d reports: %s\n  text: \"%s\"\n", reports, mt63Summary(p).c_str(), p.text.c_str());
    CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(p.text.find("LY2ABC") != std::string::npos, "text not decoded");
    CHECK(p.bandwidth == 1000 && p.interleave == 1, "bandwidth %d interleave %d", p.bandwidth, p.interleave);
    CHECK(!p.spectrum.empty(), "spectrum");
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return testSummary("mt63 engine");
}
