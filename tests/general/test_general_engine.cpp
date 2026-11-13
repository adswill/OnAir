// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// General receiver through the engine (setStandard(57)): the synthetic source plays an NFM station on the default frequency, the
// receiver reports through latestRx() with the squelch open. No sound.
#include "dect2/general/general_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = generalTuning();
    CHECK(mt.stdMode == 57 && modeTuningById("general") && modeTuningById("general")->stdMode == 57, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 57;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.general().setSilent(true);
    e.setStandard(57);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 56, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 3.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 56) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const GeneralTelemetry& g = rx.general;
    printf("  %d reports: %s\n", reports, generalSummary(g).c_str());
    CHECK(reports > 10 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(g.squelchOpen && g.dataValid && g.snrDb > 15, "squelch %d snr %.1f", g.squelchOpen, g.snrDb);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    printf(fails ? "general engine: %d FAILED\n" : "general engine: all passed\n", fails);
    return fails ? 1 : 0;
}
