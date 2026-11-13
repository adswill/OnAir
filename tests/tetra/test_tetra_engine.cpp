// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRA through the engine (setStandard(48)): the synthetic source plays the two test cells at 2 Msps; the receiver finds them and
// reports through latestRx() with their network information.
#include "dect2/engine/engine.h"
#include "dect2/engine/modes.h"
#include "dect2/tetra/tetra_gen.h"
#include "dect2/tetra/tetra_rx.h"
#include "dect2/tetra/tetra_tel.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = tetraTuning();
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = kTetraTestCentreHz;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 48;
    t.synth.snrDb = 22;
    t.synth.modeOpt[0] = 1;
    FileOptions fo;
    e.setStandard(48);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 47, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrongStd = 0;
    bool ok = false;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 60.0) {
        if (e.latestRx(rx, seq)) {
            seq = rx.seq; reports++;
            if (rx.standard != 47) wrongStd++;
            bool a = false, b = false;
            for (const auto& c : rx.tetra.cells) { if (c.colourCode == 5 && c.hasSysinfo) a = true; if (c.colourCode == 12 && c.hasSysinfo) b = true; }
            if (a && b) ok = true;
            if (ok) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    printf("  %d reports: %s\n", reports, tetraSummary(rx.tetra).c_str());
    CHECK(ok, "test cells not identified");
    for (const auto& c : rx.tetra.cells)
        if (c.colourCode == 5) CHECK(c.mcc == 1 && c.mnc == 1 && c.la == 1001 && std::fabs(c.nominalHz - 392.4125e6) < 1 && c.encryption(), "cell fields: LA %d", c.la);
    CHECK(wrongStd == 0 && rx.rateOk, "standard / rate");
    CHECK(!modeSummary(rx).empty(), "summary");
    e.stop();
    printf(fails ? "tetra engine: %d FAILED\n" : "tetra engine: all passed\n", fails);
    return fails ? 1 : 0;
}
