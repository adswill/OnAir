// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Multitone paging through the engine (setStandard(77)): the synthetic source plays the test signal (an invented one: the real format is not
// public), the receiver reports through latestRx(). No sound.
#include "dect2/engine/engine.h"
#include "dect2/multitone/multitone_rx.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = multitoneTuning();
    CHECK(mt.stdMode == 77 && std::string(mt.id) == "multitone", "tuning");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = 446.475e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 77;
    t.synth.snrDb = 30;
    FileOptions fo;
    e.setStandard(77);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 76, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 11.5) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 76) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const MultitoneTelemetry& d = rx.multitone;
    printf("  %d reports: %s\n", reports, multitoneSummary(d).c_str());
    CHECK(reports > 10 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(d.tx.size() == 2 && d.bursts.size() == 1, "%zu transmissions, %zu bursts", d.tx.size(), d.bursts.size());
    CHECK(!d.bursts.empty() && d.bursts[0].bestRun >= 8, "valid word run %d", d.bursts.empty() ? -1 : d.bursts[0].bestRun);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.multitone().clearResults();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    e.stop();
    printf(fails ? "multitone engine: %d FAILED\n" : "multitone engine: all passed\n", fails);
    return fails ? 1 : 0;
}
