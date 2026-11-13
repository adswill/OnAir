// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ARDOP through the engine (setStandard(142)): the synthetic source plays the test signal, the receiver reports through latestRx().
#include "dect2/ardop/ardop_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = ardopTuning();
    CHECK(mt.stdMode == 142 && modeTuningById("ardop") && modeTuningById("ardop")->stdMode == 142, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 142;
    t.synth.snrDb = 20;
    FileOptions fo;
    e.setStandard(142);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 141, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 36.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 141) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const ArdopTelemetry& p = rx.ardop;
    printf("  %d reports: %s\n  text: \"%s\"\n", reports, ardopSummary(p).c_str(), p.text.c_str());
    CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(p.blocksOk >= 6 && p.dataOk >= 2, "%llu frames, %llu data frames", (unsigned long long)p.blocksOk, (unsigned long long)p.dataOk);
    CHECK(p.text.find("LY2ABC") != std::string::npos, "text not decoded");
    CHECK(!p.stations.empty() && p.stations.back().call.size() >= 6, "stations");
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return testSummary("ardop engine");
}
