// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GOES HRIT through the engine (setStandard(62)): the synthetic source plays the HRIT test signal, the receiver decodes the products
// and reports through latestRx(). No sound.
#include "dect2/goes/goes_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = goesTuning();
    CHECK(mt.stdMode == 62 && modeTuningById("goes") && modeTuningById("goes")->stdMode == 62, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 62;
    t.synth.snrDb = 12;
    t.synth.cfoHz = 20000;
    FileOptions fo;
    e.setStandard(62);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 61, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 6.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 61) wrong++; }
        if (rx.goes.products.size() >= 5 && rx.goes.state == 3) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const GoesTelemetry& g = rx.goes;
    printf("  %d reports: %s, cfo %.0f Hz\n", reports, goesSummary(g).c_str(), g.cfoHz);
    CHECK(reports >= 1 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(g.state == 3 && g.products.size() >= 5, "state %d, %zu products", g.state, g.products.size());
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    printf(fails ? "goes engine: %d FAILED\n" : "goes engine: all passed\n", fails);
    return fails ? 1 : 0;
}
