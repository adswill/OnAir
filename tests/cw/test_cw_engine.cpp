// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CW through the engine (setStandard(36)): the synthetic source plays the test band, the receiver reports through latestRx().
#include "dect2/cw/cw_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = cwTuning();
    CHECK(mt.stdMode == 36 && modeTuningById("cw") && modeTuningById("cw")->stdMode == 36, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 36;
    t.synth.snrDb = 22;
    FileOptions fo;
    e.cw().setSilent(true);
    e.setStandard(36);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 35, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 14.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 35) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const CwTelemetry& c = rx.cw;
    int found = 0;
    for (const auto& s : c.signals) {
        printf("  %+7.0f Hz %4.1f WPM %4.1f dB \"%s\"\n", s.offsetHz, s.wpm, s.snrDb, s.text.c_str());
        if (s.text.find("LY2ABC") != std::string::npos || s.text.find("DL1XYZ") != std::string::npos || s.text.find("UR 599") != std::string::npos || s.text.find("OK1XX") != std::string::npos) found++;
    }
    printf("  %d reports: %s\n", reports, cwSummary(c).c_str());
    CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(found >= 3, "%d signals decoded", found);
    CHECK(!c.spectrum.empty(), "spectrum");
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    printf(fails ? "cw engine: %d FAILED\n" : "cw engine: all passed\n", fails);
    return fails ? 1 : 0;
}
