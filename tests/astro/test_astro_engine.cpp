// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Radio astronomy through the engine (setStandard(50)): the synthetic source plays the hydrogen-line test sky in real time; the running
// average and a finished 1 s integration come back through latestRx() as standard 49.
#include "dect2/astro/astro_rx.h"
#include "dect2/astro/astro_tel.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"
int main() {
    const ModeTuning mt = astroTuning();
    CHECK(mt.stdMode == 50 && modeTuningById("astro") && modeTuningById("astro")->stdMode == 50, "tuning table");
    CHECK(std::fabs(mt.defMhz - 1420.405751) < 1e-6, "default %.6f MHz", mt.defMhz);
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = 1e6;
    t.synth.mode = 50;
    FileOptions fo;
    e.setStandard(50);
    e.astro().setFftSize(2048);
    e.astro().setIntegration(1.0);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 49, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 3.5) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 49) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const AstroTelemetry& s = rx.astro;
    printf("  %d reports: %s\n", reports, astroSummary(s).c_str());
    CHECK(reports > 8 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk && s.fftSize == 2048 && s.avg.size() == 2048, "fft %d, %zu bins", s.fftSize, s.avg.size());
    CHECK(s.completed >= 1 && s.last.size() == 2048, "%llu integrations", (unsigned long long)s.completed);
    CHECK(!s.switching, "frequency switching without a radio");
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    printf(fails ? "astro engine: %d FAILED\n" : "astro engine: all passed\n", fails);
    return fails ? 1 : 0;
}
