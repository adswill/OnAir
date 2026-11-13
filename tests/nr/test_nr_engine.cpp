// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 5G NR cells through the engine (setStandard(59)): the synthetic source plays two n78 cells at 20 Msps around 3541.44 MHz; the receiver
// searches the sync raster in the block, decodes both MIBs and reports through latestRx() with their GSCN and band.
#include "dect2/engine/engine.h"
#include "dect2/engine/modes.h"
#include "dect2/nr/nr_rx.h"
#include "dect2/nr/nr_tel.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = nrTuning();
    CHECK(mt.stdMode == 59 && modeTuningById("nr") && modeTuningById("nr")->stdMode == 59, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 59;
    t.synth.snrDb = 10;
    t.synth.cfoHz = 12000;
    FileOptions fo;
    e.setStandard(59);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 58, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrongStd = 0;
    bool ok = false;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 40.0) {
        if (e.latestRx(rx, seq)) {
            seq = rx.seq; reports++;
            if (rx.standard != 58) wrongStd++;
            int got = 0;
            for (const auto& c : rx.nr.cells) if ((c.pci == 501 || c.pci == 1007) && c.hasMib) got++;
            if (got == 2) { ok = true; break; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    printf("  %d reports in %.1f s: %s\n", reports, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), nrSummary(rx.nr).c_str());
    CHECK(ok, "test cells not decoded");
    for (const auto& c : rx.nr.cells) {
        printf("  PCI %d GSCN %d n%d %.2f MHz SSB %d beams %zu SFN %d kSSB %d CORESET0 %d (%d RB) RSRP %.1f SINR %.1f cfo %.0f\n", c.pci, c.gscn, c.band, c.freqHz / 1e6, c.ssbIdx,
               c.beams.size(), c.sfn, c.kSsb, c.coreset0, c.cs0Rbs, c.rsrpDb, c.sinrDb, c.cfoHz);
        if (c.pci == 501) CHECK(c.gscn == 7872 && c.band == 78 && c.kSsb == 4 && c.coreset0 == 12 && c.ss0 == 2 && c.cs0Rbs == 48 && c.sib1Scheduled && c.lmax == 8, "cell 501 fields");
        if (c.pci == 1007) CHECK(c.gscn == 7878 && c.ssbIdx == 4 && c.kSsb == 18, "cell 1007 fields");
        CHECK(std::fabs(c.cfoHz - 12000) < 400, "cfo %.0f", c.cfoHz);
    }
    CHECK(wrongStd == 0 && rx.rateOk, "standard / rate");
    CHECK(!modeSummary(rx).empty(), "summary");
    e.stop();
    printf(fails ? "nr engine: %d FAILED\n" : "nr engine: all passed\n", fails);
    return fails ? 1 : 0;
}
