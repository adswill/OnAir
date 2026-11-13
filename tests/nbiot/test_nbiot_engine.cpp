// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NB-IoT cells through the engine (setStandard(71)): the synthetic source plays the standalone test cell, the receiver reports through latestRx(),
// the results save and load. No sound.
#include "dect2/nbiot/nbiot_rx.h"
#include "dect2/nbiot/nbiot_tel.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = nbiotTuning();
    CHECK(mt.stdMode == 71 && modeTuningById("nbiot") && modeTuningById("nbiot")->stdMode == 71, "tuning table");
    CHECK(minSampleRateFor(71, 2) == 1e6, "minimum rate");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = 806e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 71;
    t.synth.snrDb = 20;
    t.synth.modeOpt[0] = 0;
    FileOptions fo;
    e.setStandard(71);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 70, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    bool got = false;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 45.0) {
        if (e.latestRx(rx, seq)) {
            seq = rx.seq; reports++; if (rx.standard != 70) wrong++;
            for (const auto& c : rx.nbiot.cells) if (c.hasSib1 && c.hasMib) got = true;
        }
        if (got) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const NbiotTelemetry& d = rx.nbiot;
    printf("  %d reports: %s, %zu SIB1-NB messages\n", reports, nbiotSummary(d).c_str(), (size_t)d.blocksOk);
    CHECK(reports > 3 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(got && d.cells.size() == 1, "%zu cells", d.cells.size());
    if (got && !d.cells.empty()) {
        const NbCell& c = d.cells[0];
        CHECK(c.pci == 100 && c.mode == 3 && c.ports == 1 && c.mcc == 1 && c.mnc == 1 && c.tac == 0x1A2B && c.cellId == 0x0123456 && c.band == 8, "cell fields: PCI %d mode %d TAC %u cell %u band %d", c.pci, c.mode, c.tac, c.cellId, c.band);
        CHECK(std::fabs(c.offsetKhz - 2.5) < 1.0, "raster offset %.2f kHz", c.offsetKhz);
        CHECK(c.earfcn >= 0 && c.freqHz == 806e6, "raster frequency %.0f, EARFCN %d", c.freqHz, c.earfcn);
        CHECK(d.events.size() >= 2, "events");
    }
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    // results persistence: what was saved loads into a new receiver
    const std::string blob = e.nbiot().saveResults();
    e.nbiot().clearResults();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    NbiotReceiver r2;
    CHECK(r2.loadResults(blob), "load");
    NbiotTelemetry tl;
    CHECK(r2.telemetry(tl, 0) && tl.cells.size() == (got ? 1u : 0u), "reloaded cells %zu", tl.cells.size());
    if (!tl.cells.empty()) CHECK(tl.cells[0].pci == 100 && tl.cells[0].hasSib1 && tl.cells[0].plmns.size() == 1 && tl.cells[0].plmns[0] == "001-01", "reloaded cell");
    CHECK(!r2.loadResults("not a results file"), "garbage refused");
    e.stop();
    printf(fails ? "nbiot engine: %d FAILED\n" : "nbiot engine: all passed\n", fails);
    return fails ? 1 : 0;
}
