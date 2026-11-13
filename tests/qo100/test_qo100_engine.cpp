// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// QO-100 through the engine (setStandard(55)): the synthetic source plays the narrowband transponder at 2 Msps in real time; the beacons
// lock and the reports come back through latestRx() as standard 54 with the LNB error.
#include "dect2/engine/engine.h"
#include "dect2/qo100/qo100_dsp.h"
#include "dect2/qo100/qo100_gen.h"
#include "dect2/qo100/qo100_rx.h"
#include "dect2/qo100/qo100_tel.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"
int main() {
    const ModeTuning mt = qo100Tuning();
    CHECK(mt.stdMode == 55 && modeTuningById("qo100") && modeTuningById("qo100")->stdMode == 55, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = qo100::ifOf(qo100::kNarrowCentreHz, 9750e6);
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = 2e6;
    t.synth.mode = 55;
    FileOptions fo;
    e.setStandard(55);
    e.qo100().setLo(9750e6);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 54, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    bool locked = false;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 7.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 54) wrong++; if (rx.qo100.state == 2) locked = true; }
        if (locked && reports > 20) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const Qo100Telemetry& s = rx.qo100;
    printf("  %d reports: %s\n", reports, qo100Summary(s).c_str());
    CHECK(reports > 8 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk && s.nbOk, "rate / band");
    CHECK(locked && s.corrValid && std::fabs(s.lnbErrHz - kQo100SynthErrHz) < 400, "lock %d, LNB error %.1f Hz", locked, s.lnbErrHz);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    printf(fails ? "qo100 engine: %d FAILED\n" : "qo100 engine: all passed\n", fails);
    return fails ? 1 : 0;
}
