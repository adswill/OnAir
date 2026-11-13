// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// JS8Call through the engine (setStandard(39)): the synthetic source plays the test stations in step with the system clock, the
// receiver takes its slots from the same clock and reports through latestRx().
#include "dect2/js8/js8_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = js8Tuning();
    CHECK(mt.stdMode == 39 && modeTuningById("js8") && modeTuningById("js8")->stdMode == 39, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 39;
    t.synth.snrDb = 4;
    FileOptions fo;
    e.setStandard(39);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 38, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 26.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 38) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const Js8Telemetry& j = rx.js8;
    int fastTurbo = 0;
    for (const auto& d : j.decodes) {
        printf("  %s %+5.1f dB %+5.2f s %6.0f Hz  %s\n", d.submode == 0 ? "N" : d.submode == 1 ? "F" : "T", d.snrDb, d.dt, d.hz, d.text.c_str());
        if (d.from == "YL2XYZ" || d.from == "ES1ABC") fastTurbo++;
    }
    printf("  %d reports: %s\n", reports, js8Summary(j).c_str());
    CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(fastTurbo >= 2, "%d fast / turbo decodes", fastTurbo);
    CHECK(!j.spectrum.empty(), "spectrum");
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    printf(fails ? "js8 engine: %d FAILED\n" : "js8 engine: all passed\n", fails);
    return fails ? 1 : 0;
}
