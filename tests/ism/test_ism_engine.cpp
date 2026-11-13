// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ISM sensors through the engine (setStandard(30)): the synthetic source plays the 433.92 MHz test scene, the receiver hears its devices
// and reports through latestRx() with their real frequencies; a retune keeps the report numbers growing and starts a fresh device list.
#include "dect2/engine/engine.h"
#include "dect2/ism/ism_gen.h"
#include "dect2/ism/ism_rx.h"
#include "dect2/engine/modes.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = ismTuning();
    CHECK(mt.stdMode == 30 && modeTuningById("ism") && modeTuningById("ism")->stdMode == 30, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 30;
    t.synth.snrDb = 30;
    FileOptions fo;
    e.setStandard(30);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 29, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrongStd = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 8.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 29) wrongStd++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const IsmTelemetry& m = rx.ism;
    int right = 0, wrong = 0;
    for (const auto& d : m.devices) {
        IsmReading r; r.model = d.model; r.id = d.id; r.fields = d.fields;
        IsmTx w;
        if (ismTruthMatches(r, &w)) {
            right++;
            for (const auto& g : ismTestScene(0))
                if (g.proto == w) CHECK(std::fabs(d.freqHz - (t.centerHz + g.offsetHz)) < 8e3, "%s at %.4f MHz", d.model.c_str(), d.freqHz / 1e6);
        } else wrong++;
    }
    printf("  %d reports: %s; %d right, %d wrong, centre %.4f MHz\n", reports, ismSummary(m).c_str(), right, wrong, m.centerHz / 1e6);
    CHECK(reports > 10 && wrongStd == 0, "%d reports, %d wrong standard", reports, wrongStd);
    CHECK(rx.rateOk, "rate");
    CHECK(std::fabs(m.centerHz - (t.centerHz + mt.tuneOffsetHz)) < 1, "centre %.0f", m.centerHz);
    CHECK(right >= 12 && wrong == 0, "%d right, %d wrong devices", right, wrong);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    CHECK(!modeSummary(rx).empty(), "summary");
    const uint64_t before = rx.seq;
    TuneSettings t2 = t; t2.centerHz = 868.3e6;
    e.retuneReset(t2);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    e.latestRx(rx, 0);
    CHECK(rx.seq > before && std::fabs(rx.ism.centerHz - (868.3e6 + mt.tuneOffsetHz)) < 1, "after retune: seq %llu, centre %.0f", (unsigned long long)rx.seq, rx.ism.centerHz);
    e.stop();
    printf(fails ? "ism engine: %d FAILED\n" : "ism engine: all passed\n", fails);
    return fails ? 1 : 0;
}
