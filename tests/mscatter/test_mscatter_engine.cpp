// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meteor scatter through the engine (setStandard(106)): the synthetic source plays the beacon with echoes and the receiver reports through latestRx().
#include "dect2/engine/engine.h"
#include "dect2/mscatter/mscatter_live.h"
#include "dect2/mscatter/mscatter_tel.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = mscatterTuning();
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6; t.bandwidthMhz = mt.bandwidthMhz; t.sampleRate = mt.sampleRate;
    t.synth.mode = 106; t.synth.snrDb = 40;
    FileOptions fo;
    e.setStandard(106);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 105, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    bool got = false;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 50) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; if (rx.standard == 105 && rx.mscatter.pings.size() >= 3) { got = true; break; } }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    printf("  %s, beacon %+.1f Hz\n", msSummary(rx.mscatter).c_str(), rx.mscatter.cfoHz);
    CHECK(got, "fewer than 3 pings in 50 s");
    CHECK(std::fabs(rx.mscatter.cfoHz - 420) < 10, "beacon %.1f Hz", rx.mscatter.cfoHz);
    CHECK(rx.rateOk && rx.dataValid, "rate %d valid %d", (int)rx.rateOk, (int)rx.dataValid);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return testSummary("mscatter engine");
}
