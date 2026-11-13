// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VDES through the engine (setStandard(65)): the synthetic source plays ASM and VDE-TER bursts, the receiver reports them through latestRx().
#include "dect2/engine/engine.h"
#include "dect2/engine/modes.h"
#include "dect2/vdes/vdes_rx.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = vdesTuning();
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 65;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.setStandard(65);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 64, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 8.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 64) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const VdesTelemetry& v = rx.vdes;
    printf("  %d reports: %s, %zu ASM, %zu bursts\n", reports, vdesSummary(v).c_str(), v.asms.size(), v.bursts.size());
    for (size_t i = 0; i < v.bursts.size() && i < 8; i++) printf("    burst %.2f s: %d kHz %s cv %.2f psi %+.2f cfo %+.0f SNR %.1f\n", v.bursts[i].timeSec, v.bursts[i].bwKhz, vdeModName(v.bursts[i].mod), v.bursts[i].cv, v.bursts[i].psi, v.bursts[i].cfoHz, v.bursts[i].snrDb);
    CHECK(reports > 10 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(v.asms.size() >= 6 && v.bursts.size() >= 6, "%zu ASM, %zu bursts", v.asms.size(), v.bursts.size());
    CHECK(v.vdeByMod[1] > 0 && v.vdeByMod[2] > 0 && v.vdeByMod[3] > 0, "modulations %llu %llu %llu", (unsigned long long)v.vdeByMod[1], (unsigned long long)v.vdeByMod[2], (unsigned long long)v.vdeByMod[3]);
    CHECK(!modeSummary(rx).empty(), "summary");
    e.stop();
    printf(fails ? "FAILED: %d\n" : "vdes engine ok\n", fails);
    return fails ? 1 : 0;
}
