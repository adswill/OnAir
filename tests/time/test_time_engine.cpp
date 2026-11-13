// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Time signals through the engine (setStandard(60)): the synthetic source plays DCF77 at the computer's time, the receiver finds the
// seconds and reports through latestRx(). A whole minute takes too long for this test; test_time_rx decodes them. No sound.
#include "dect2/time/time_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = timeTuning();
    CHECK(mt.stdMode == 60 && modeTuningById("time") && modeTuningById("time")->stdMode == 60, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 60;
    t.synth.snrDb = 30;
    FileOptions fo;
    e.setStandard(60);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 59, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 8.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 59) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const TimeTelemetry& d = rx.timesig;
    printf("  %d reports: %s, depth %.2f, tick %.2f ms rms, station %d\n", reports, timeSummary(d).c_str(), d.depth, d.tickJitterMs, d.station);
    CHECK(reports > 10 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(d.station == 0 && d.state >= 1 && d.depth > 0.5, "DCF77 seconds not found");
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    printf(fails ? "time engine: %d FAILED\n" : "time engine: all passed\n", fails);
    return fails ? 1 : 0;
}
