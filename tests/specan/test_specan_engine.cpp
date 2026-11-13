// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The spectrum analyser through the engine (setStandard(44)) with the synthetic source, which plays a HackRF native sweep of
// 1 - 6000 MHz with carriers at 100, 433.92, 1090 and 2440 MHz. No radio.
#include "dect2/specan/specan_rx.h"
#include "dect2/specan/specan_gen.h"
#include "dect2/specan/specan_dsp.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = specanTuning();
    CHECK(mt.stdMode == 44 && modeTuningById("specan") && modeTuningById("specan")->stdMode == 44, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 44;
    t.synth.snrDb = 40;
    t.synth.modeVal[0] = 1; t.synth.modeVal[1] = 6000;
    FileOptions fo;
    e.setStandard(44);
    e.specan().setRange(1e6, 6000e6);
    e.specan().setBinHz(100e3);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 43, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 6.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 43) wrong++; }
        if (rx.specan.blocksOk >= 4) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const SpecanTelemetry& s = rx.specan;
    printf("  %d reports: %s; %d steps, %zu bins of %.0f Hz, gaps %llu\n", reports, specanSummary(s).c_str(), s.steps, s.live.size(), s.binHz,
           (unsigned long long)s.blocksBad);
    // the engine can finish all four sweeps before the first poll (a fast Mac), so one report is enough; the loop has already waited for 4 sweeps
    CHECK(reports >= 1 && rx.specan.blocksOk >= 4 && wrong == 0 && s.native, "%d reports, %d wrong, native %d", reports, wrong, s.native);
    CHECK(rx.rateOk, "rate");
    if (!s.live.empty()) {
        SaGrid g = saMakeGrid(s.loHz, s.loHz + s.binHz * s.live.size(), s.binHz);
        int nt = 0;
        const SpecanTone* tn = specanTestTones(nt);
        for (int k = 0; k < nt; k++) {
            float m = -200;
            for (long i = g.index(tn[k].hz) - 1; i <= g.index(tn[k].hz) + 1; i++) m = std::max(m, s.live[(size_t)i]);
            printf("  %.2f MHz: %.1f dBFS (sent %.0f)\n", tn[k].hz / 1e6, m, tn[k].dbfs);
            CHECK(std::fabs(m - tn[k].dbfs) < 4, "tone %.2f MHz %.1f", tn[k].hz / 1e6, m);
        }
        CHECK(s.live[(size_t)g.index(3000e6)] < -70, "floor %.1f", s.live[(size_t)g.index(3000e6)]);
        CHECK(s.blocksBad == 0 || s.blocksOk > s.blocksBad, "gaps %llu", (unsigned long long)s.blocksBad);
        CHECK(s.sweepMs > 50 && s.sweepMs < 1500, "sweep %.0f ms", s.sweepMs);
    } else CHECK(false, "no sweep");
    e.stop();
    printf(fails ? "specan engine: %d FAILED\n" : "specan engine: all passed\n", fails);
    return fails ? 1 : 0;
}
