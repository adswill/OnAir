// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Globalstar through the engine (setStandard(64)): the synthetic source plays a pass, the receiver reports channels and the Doppler track through latestRx().
#include "dect2/engine/engine.h"
#include "dect2/engine/modes.h"
#include "dect2/globalstar/globalstar_rx.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = globalstarTuning();
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = 8e6;
    t.synth.mode = 64;
    t.synth.snrDb = 15;
    t.synth.modeVal[0] = 4;
    FileOptions fo;
    e.setStandard(64);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 63, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 6.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 63) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const GlobalstarTelemetry& g = rx.globalstar;
    printf("  %d reports: %s, %zu points, %zu events\n", reports, globalstarSummary(g).c_str(), g.track.size(), g.events.size());
    for (const GsChannel& c : g.chan) if (c.inView) printf("    ch %2d %.2f MHz active %d SNR %.1f dop %+.0f occ %.2f\n", c.index + 1, c.centreMhz, c.active, c.snrDb, c.dopplerHz, c.occupancy);
    CHECK(reports > 10 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(g.activeNow >= 1 && g.track.size() > 5, "no activity (%d active, %zu points)", g.activeNow, g.track.size());
    CHECK(!modeSummary(rx).empty(), "summary");
    e.stop();
    printf(fails ? "FAILED: %d\n" : "globalstar engine ok\n", fails);
    return fails ? 1 : 0;
}
