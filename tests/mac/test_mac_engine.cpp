// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MAC through the engine (setStandard(49)): the synthetic source plays the D2-MAC test signal at 20 Msps, the receiver reports through
// latestRx() and makes pictures. Silent: the receiver never opens the sound device.
#include "dect2/mac/mac_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = macTuning();
    CHECK(mt.stdMode == 49 && modeTuningById("mac") && modeTuningById("mac")->stdMode == 49, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 49;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.mac().setSilent(true);
    e.setStandard(49);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 48, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0, fseq = 0;
    int reports = 0, wrong = 0, pictures = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 10.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 48) wrong++; }
        if (e.mac().frame(fseq)) pictures++;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const MacTelemetry& m = rx.mac;
    printf("  %d reports: %s, %d pictures, dropped %llu\n", reports, macSummary(m).c_str(), pictures, (unsigned long long)e.droppedSamples());
    CHECK(reports > 10 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(m.state == 2 && m.channelName == "ONAIR TV", "state %d name '%s'", m.state, m.channelName.c_str());
    CHECK(pictures > 5, "%d pictures", pictures);
    e.stop();
    printf(fails ? "mac engine: %d FAILED\n" : "mac engine: all passed\n", fails);
    return fails ? 1 : 0;
}
