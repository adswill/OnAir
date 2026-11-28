// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CATS through the engine (setStandard(902)): the synthetic source plays packets, the receiver reports through latestRx().
#include "dect2/cats/cats_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"
#include "../common/test_long.h"

int main() {
    const ModeTuning mt = catsTuning();
    CHECK(mt.stdMode == 902 && modeTuning(902) && modeTuningById("cats") && modeTuningById("cats")->stdMode == 902, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = 250000;
    t.synth.mode = 902;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.setStandard(902);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 901, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    auto passed = [&] { return reports > 5 && wrong == 0 && rx.cats.blocksOk >= 2 && e.droppedSamples() == 0; };
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 45.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 901) wrong++; }
        if (!testLong() && passed()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const CatsTelemetry& p = rx.cats;
    printf("  %d reports: %s\n", reports, catsSummary(p).c_str());
    CHECK(reports > 5 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(p.blocksOk >= 2 && !p.packets.empty(), "%llu packets", (unsigned long long)p.blocksOk);
    if (!p.packets.empty()) {
        const CatsPacketRow& r = p.packets[0];
        CHECK(r.callsign == "TEST" && r.ssid == 1 && r.hasGps && std::fabs(r.lat - 52.25) < 1e-4 && r.comment == "OnAir test balloon", "packet fields %s-%d %.5f", r.callsign.c_str(), r.ssid, r.lat);
    }
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return testSummary("cats engine");
}
