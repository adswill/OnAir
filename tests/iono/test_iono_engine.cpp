// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ionospheric sounders through the engine (setStandard(54)): the synthetic source plays the two-sounder test scene in real time at
// 1 Msps around 0.5 MHz; the 100 kHz/s sweep that starts at 2 s comes back through latestRx() as standard 53 with its ionogram.
#include "dect2/iono/iono_dsp.h"
#include "dect2/iono/iono_gen.h"
#include "dect2/iono/iono_rx.h"
#include "dect2/iono/iono_tel.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"
int main() {
    const ModeTuning mt = ionoTuning();
    CHECK(mt.stdMode == 54 && modeTuningById("iono") && modeTuningById("iono")->stdMode == 54, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = 0.5e6;
    t.bandwidthMhz = 1;
    t.sampleRate = 1e6;
    t.synth.mode = 54;
    t.synth.modeVal[0] = 0.5;
    FileOptions fo;
    e.setStandard(54);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 53, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 4.5) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 53) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const IonoTelemetry& s = rx.iono;
    printf("  %d reports: %s\n", reports, ionoSummary(s).c_str());
    CHECK(reports > 8 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk && s.state == 1 && s.covered.size() == (size_t)iono::kCols, "state %d", s.state);
    CHECK(s.sounders.size() == 1 && s.sounders[0].rateHz == 100e3, "%zu sounders", s.sounders.size());
    CHECK(s.sel == 0 && s.ionoDb.size() == (size_t)iono::kRows * iono::kCols, "ionogram %zu", s.ionoDb.size());
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    // Clear (#244): the very next telemetry must not name a sounder that is gone (the display indexed sounders[sel] and crashed)
    e.iono().clearResults();
    RxTelemetry after;
    bool got = false;
    // (a telemetry frame assembled just before the clear can still arrive once: skip frames until one is newer than the clear)
    for (int i = 0; i < 150; i++) {
        const bool g = e.latestRx(after, seq);
        if (g) { got = true; if (after.iono.sounders.empty()) break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(got, "no telemetry after Clear");
    CHECK(after.iono.sounders.empty() && after.iono.sel == -1, "after Clear: %zu sounders, sel %d", after.iono.sounders.size(), after.iono.sel);
    e.stop();
    printf(fails ? "iono engine: %d FAILED\n" : "iono engine: all passed\n", fails);
    return fails ? 1 : 0;
}
