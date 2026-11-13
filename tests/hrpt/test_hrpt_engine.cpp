// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HRPT / AHRPT through the engine (setStandard(66)): the synthetic source plays the MetOp test signal, the receiver decodes lines and
// reports through latestRx(). No sound.
#include "dect2/hrpt/hrpt_rx.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = hrptTuning();
    CHECK(mt.stdMode == 66 && modeTuningById("hrpt") && modeTuningById("hrpt")->stdMode == 66, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 66;
    t.synth.snrDb = 14;
    t.synth.cfoHz = 20000;
    FileOptions fo;
    e.setStandard(66);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 65, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 20.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 65) wrong++; }
        if (rx.hrpt.lines >= 4 && rx.hrpt.state == 3) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const HrptTelemetry& h = rx.hrpt;
    printf("  %d reports: %s, cfo %.0f Hz\n", reports, hrptSummary(h).c_str(), h.cfoHz);
    CHECK(reports >= 1 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(h.state == 3 && h.lines >= 4, "state %d, %llu lines", h.state, (unsigned long long)h.lines);
    e.stop();
    printf(fails ? "hrpt engine: %d FAILED\n" : "hrpt engine: all passed\n", fails);
    return fails ? 1 : 0;
}
