// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FPV video through the engine (setStandard(103)): the synthetic source plays the test transmitter, the receiver reports through latestRx().
#include "dect2/engine/engine.h"
#include "dect2/fpv/fpv_live.h"
#include "dect2/fpv/fpv_tel.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = fpvTuning();
    CHECK(mt.stdMode == 103 && std::string(mt.id) == "fpv", "tuning");
    CHECK(modeTuning(103) && modeTuning(103)->sampleRate == 20e6, "registered");
    CHECK(modeTuningById("fpv") != nullptr, "by id");
    Engine e;
    e.fpv().setSilent(true);
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6; t.bandwidthMhz = mt.bandwidthMhz; t.sampleRate = mt.sampleRate;
    t.synth.mode = 103; t.synth.snrDb = 30;
    FileOptions fo;
    e.setStandard(103);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 102, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    bool got = false;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 40) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; if (rx.standard == 102 && rx.fpv.state == 2 && rx.dataValid) { got = true; break; } }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    printf("  %s\n", fpvSummary(rx.fpv).c_str());
    CHECK(got, "no locked picture in 40 s");
    CHECK(rx.fpv.tunedName == "R5", "channel '%s'", rx.fpv.tunedName.c_str());
    CHECK(rx.rateOk, "rate");
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    uint64_t fs = 0;
    CHECK(e.fpv().frame(fs) != nullptr, "no picture");
    e.stop();
    return testSummary("fpv engine");
}
