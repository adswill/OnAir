// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NXDN through the engine (setStandard(40)): the synthetic source plays the test schedule, the receiver reports through latestRx().
// Silent: the receiver never opens the sound device.
#include "dect2/nxdn/nxdn_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = nxdnTuning();
    CHECK(mt.stdMode == 40 && modeTuningById("nxdn") && modeTuningById("nxdn")->stdMode == 40, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 40;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.nxdn().setSilent(true);
    e.setStandard(40);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 39, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 12.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 39) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const NxdnTelemetry& d = rx.nxdn;
    printf("  %d reports: %s, %zu log entries, %llu frames, site %s\n", reports, nxdnSummary(d).c_str(), d.log.size(), (unsigned long long)d.frames, d.siteInfo.valid ? "yes" : "no");
    CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(d.log.size() >= 4 && d.siteInfo.valid, "%zu log entries", d.log.size());
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    printf(fails ? "nxdn engine: %d FAILED\n" : "nxdn engine: all passed\n", fails);
    return fails ? 1 : 0;
}
