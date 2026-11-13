// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AM stereo through the engine (setStandard(109)): the synthetic source plays the test signal, the receiver reports through latestRx().
#include "dect2/cquam/cquam_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = cquamTuning();
    CHECK(mt.stdMode == 109 && modeTuningById("cquam") && modeTuningById("cquam")->stdMode == 109, "tuning table");
    Engine e;
    e.cquam().setSilent(true);
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 109;
    t.synth.snrDb = 28;
    FileOptions fo;
    e.setStandard(109);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 108, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 14.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 108) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const CquamTelemetry& p = rx.cquam;
    printf("  %d reports: %s\n", reports, cquamSummary(p).c_str());
    CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(p.state == 2 && p.stereo, "state %d", p.state);
    CHECK(p.pilotPct > 3.5f && p.pilotPct < 6.5f, "pilot %.2f %%", p.pilotPct);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return testSummary("cquam engine");
}
