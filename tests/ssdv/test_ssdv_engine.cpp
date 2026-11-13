// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SSDV through the engine (setStandard(208)): the synthetic source plays the test signal, the receiver reports through latestRx().
#include "dect2/ssdv/ssdv_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = ssdvTuning();
    CHECK(mt.stdMode == 208 && modeTuningById("ssdv") && modeTuningById("ssdv")->stdMode == 208 && modeTuning(208), "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = 1e6;
    t.synth.mode = 208;
    t.synth.snrDb = 18;
    t.synth.modeVal[0] = 9600;
    FileOptions fo;
    e.setStandard(208);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 207, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 16.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 207) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const SsdvTelemetry& s = rx.ssdv;
    printf("  %d reports: %s\n", reports, ssdvSummary(s).c_str());
    CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(s.blocksOk >= 5 && !s.images.empty() && s.images[0].jpeg && !s.images[0].jpeg->empty(), "%llu packets, %zu pictures", (unsigned long long)s.blocksOk, s.images.size());
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return testSummary("ssdv engine");
}
