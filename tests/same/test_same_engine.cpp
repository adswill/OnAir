// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NOAA weather radio SAME through the engine (setStandard(78)): the synthetic source plays the test signal, the receiver reports through latestRx().
// No sound.
#include "dect2/engine/engine.h"
#include "dect2/same/same_rx.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = sameTuning();
    CHECK(mt.stdMode == 78 && modeTuningById("same") && modeTuningById("same")->stdMode == 78, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = 162.55e6 + mt.tuneOffsetHz;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 78;
    t.synth.snrDb = 25;
    t.synth.modeOpt[0] = 2;      // the tornado warning alone
    FileOptions fo;
    e.setStandard(78);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 77, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 14.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 77) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const SameTelemetry& d = rx.same;
    printf("  %d reports: %s\n", reports, sameSummary(d).c_str());
    CHECK(reports > 10 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    bool tor = false;
    for (const auto& a : d.alerts) if (a.valid && !a.eom && a.msg.event == "TOR" && a.msg.locs.size() == 3) tor = true;
    CHECK(tor, "no tornado warning among %zu messages", d.alerts.size());
    CHECK(d.blocksOk >= 2, "%llu messages", (unsigned long long)d.blocksOk);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.same().clearResults();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    e.stop();
    printf(fails ? "same engine: %d FAILED\n" : "same engine: all passed\n", fails);
    return fails ? 1 : 0;
}
