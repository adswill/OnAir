// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GOES DCS through the engine (setStandard(143)): the synthetic source plays three platforms and the receiver reports through latestRx().
#include "dect2/engine/engine.h"
#include "dect2/goesdcs/goesdcs_live.h"
#include "dect2/goesdcs/goesdcs_tel.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = goesdcsTuning();
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6; t.bandwidthMhz = mt.bandwidthMhz; t.sampleRate = mt.sampleRate;
    t.synth.mode = 143; t.synth.snrDb = 36;
    FileOptions fo;
    e.setStandard(143);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 142, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    bool got = false;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 60) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; if (rx.standard == 142 && rx.goesdcs.platforms.size() >= 3) { got = true; break; } }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    printf("  %s\n", dcsSummary(rx.goesdcs).c_str());
    CHECK(got, "fewer than 3 platforms in 60 s");
    CHECK(rx.rateOk && rx.dataValid, "rate %d valid %d", (int)rx.rateOk, (int)rx.dataValid);
    for (const auto& m : rx.goesdcs.messages) CHECK(m.channel == 60 || m.channel == 100 || m.channel == 140, "channel %d", m.channel);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return testSummary("goesdcs engine");
}
