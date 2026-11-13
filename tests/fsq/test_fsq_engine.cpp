// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FSQ through the engine (setStandard(115)): the synthetic source plays the test signal, the receiver reports through latestRx().
#include "dect2/fsq/fsq_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = fsqTuning();
    CHECK(mt.stdMode == 115 && modeTuningById("fsq") && modeTuningById("fsq")->stdMode == 115, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 115;
    t.synth.snrDb = 8;
    FileOptions fo;
    e.setStandard(115);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 114, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 45.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 114) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const FsqTelemetry& p = rx.fsq;
    printf("  %d reports: %s\n  text: \"%s\"\n", reports, fsqSummary(p).c_str(), p.text.c_str());
    CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(p.text.find("snr please") != std::string::npos || !p.msgs.empty(), "text not decoded");
    CHECK(p.speed >= 0, "speed %d", p.speed);
    CHECK(!p.spectrum.empty(), "spectrum");
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return testSummary("fsq engine");
}
