// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cell towers through the engine (setStandard(34)): the synthetic source plays the 10 MHz test cell at 20 Msps; the receiver finds it,
// reads its MIB and system information and reports through latestRx() with its EARFCN.
#include "dect2/cell/cell_rx.h"
#include "dect2/cell/cell_tel.h"
#include "dect2/engine/engine.h"
#include "dect2/engine/modes.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = cellTuning();
    CHECK(mt.stdMode == 34 && modeTuningById("cell") && modeTuningById("cell")->stdMode == 34, "tuning table");
    CHECK(lteHzToEarfcn(806e6) == 6300 && std::fabs(lteEarfcnToHz(6200) - 796e6) < 1 && lteBandOfEarfcn(3500) == 8, "EARFCN table");
    {   // frequency -> EARFCN / band for known channels (36.101: N = Noffs + 10 (F - Flow)), and back
        struct K { double mhz; int earfcn, band; };
        const K k[] = {{1845, 1600, 3}, {1842.5, 1575, 3}, {791, 6150, 20}, {806, 6300, 20}, {2630, 2850, 7}, {943, 3630, 8}, {2120, 100, 1},
                       {2140, 300, 1}, {1960, 900, 2}, {3500, 42590, 42}, {2600, 37750 + 300, 38}};
        bool ok = true;
        for (const auto& x : k) {
            const int e = lteNearestEarfcn(x.mhz * 1e6), b = lteBandOfEarfcn(e);
            if (e != x.earfcn || b != x.band || std::fabs(lteEarfcnToHz(x.earfcn) - x.mhz * 1e6) > 1) { ok = false; printf("  %.1f MHz -> %d B%d (want %d B%d)\n", x.mhz, e, b, x.earfcn, x.band); }
        }
        CHECK(ok, "frequency <-> EARFCN known values");
        const auto both = lteBandsOfHz(2120e6);
        CHECK(both.size() >= 3 && both[0] == 1, "several bands hold 2120 MHz, B1 first (%zu)", both.size());
        CHECK(lteNearestEarfcn(1000e6) == -1 && lteBandsOfHz(1000e6).empty(), "outside every band");
        CHECK(lteNearestEarfcn(1845.02e6) == 1600 && lteNearestEarfcn(1844.96e6) == 1600, "off-raster by under 50 kHz rounds to the channel");
    }
    {   // the full 36.101 table: EARFCN ranges never overlap, both edges map to the band's downlink edges and back
        bool ok = true;
        const auto& t = lteBands();
        for (size_t i = 0; i < t.size(); i++) {
            const int span = (int)std::lround((t[i].dlHighMhz - t[i].dlLowMhz) * 10), first = t[i].nOffsDl, last = first + span - 1;
            if (span <= 0 || std::fabs(lteEarfcnToHz(first) - t[i].dlLowMhz * 1e6) > 1 || std::fabs(lteEarfcnToHz(last) - (t[i].dlHighMhz - 0.1) * 1e6) > 1) ok = false;
            if (lteBandOfEarfcn(first) != t[i].band || lteBandOfEarfcn(last) != t[i].band) ok = false;
            if (lteHzToEarfcn(t[i].dlLowMhz * 1e6, t[i].band) != first) ok = false;
            for (size_t j = 0; j < t.size(); j++)
                if (j != i && t[j].band == t[i].band) ok = false;
        }
        CHECK(ok && t.size() >= 60, "EARFCN ranges consistent");
        CHECK(std::fabs(lteEarfcnToHz(43590) - 3600e6) < 1 && std::fabs(lteEarfcnToHz(68586) - 617e6) < 1 && std::fabs(lteEarfcnToHz(3800) - 1844.9e6) < 1, "new bands");
        CHECK(lteHzToEarfcn(3700e6) == 44590 && lteHzToEarfcn(1844.9e6) == 1599 && lteHzToEarfcn(1844.9e6, 9) == 3800 && lteHzToEarfcn(1000e6) == -1, "band preference");
        CHECK(lteBandOfEarfcn(70600) == 88 && lteBandOfEarfcn(4960) == 0 && lteEarfcnToHz(4960) == 0, "gaps");
    }
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = 806e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 34;
    t.synth.snrDb = 25;
    FileOptions fo;
    e.setStandard(34);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 33, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrongStd = 0;
    bool ok = false;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 25.0) {
        if (e.latestRx(rx, seq)) {
            seq = rx.seq; reports++;
            if (rx.standard != 33) wrongStd++;
            for (const auto& c : rx.cell.cells) if (c.pci == 123 && c.hasSib1 && c.hasSib2) ok = true;
            if (ok) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    printf("  %d reports: %s\n", reports, cellSummary(rx.cell).c_str());
    CHECK(ok, "test cell not identified");
    for (const auto& c : rx.cell.cells)
        if (c.pci == 123) CHECK(c.earfcn == 6300 && c.mcc == 1 && c.mnc == 1 && c.enb == 54321 && c.sector == 2 && c.nRb == 50, "cell fields: EARFCN %d", c.earfcn);
    CHECK(wrongStd == 0 && rx.rateOk, "standard / rate");
    CHECK(!modeSummary(rx).empty(), "summary");
    e.stop();
    printf(fails ? "cell engine: %d FAILED\n" : "cell engine: all passed\n", fails);
    return fails ? 1 : 0;
}
