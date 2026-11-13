// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GSM-R receiver on the test signal: carriers found in the spectrum, synchronised, System Information decoded; frequency error, noise,
// the band edge, three cells of different levels, a radio with a lower sample rate.
#include "dect2/gsmr/gsmr_gen.h"
#include "dect2/gsmr/gsmr_rx.h"
#include "dect2/gsmr/gsmr_tel.h"
#include "dect2/source/source.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

static const GsmrCell* find(const GsmrTelemetry& t, int arfcn) {
    for (const auto& c : t.cells) if (c.arfcn == arfcn) return &c;
    return nullptr;
}

// Feed the test signal for `blocks` capture blocks and return what the receiver found
static GsmrTelemetry run(int scene, double snrDb, double cfoHz, double rate, int blocks) {
    SynthConfig sc;
    sc.mode = 41; sc.snrDb = snrDb; sc.cfoHz = cfoHz; sc.modeOpt[0] = scene;
    auto syn = makeGsmrSynth(sc, rate);
    GsmrReceiver rx;
    rx.configure(rate);
    rx.setCenterHz(kGsmrTestCentreHz);
    const size_t chunk = (size_t)(rate * 0.05);
    std::vector<cf32> buf(chunk);
    uint64_t guard = 0;
    GsmrTelemetry t;
    for (;;) {
        syn->generate(buf.data(), chunk);
        rx.feed(buf.data(), chunk);
        while (rx.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        rx.telemetry(t, 0);
        if (t.blocks >= (uint64_t)blocks || ++guard > 4000) break;
    }
    rx.feed(buf.data(), 1);
    rx.telemetry(t, 0);
    return t;
}

int main() {
    const ModeTuning mt = gsmrTuning();
    CHECK(mt.stdMode == 41 && modeTuningById("gsmr") && modeTuningById("gsmr")->stdMode == 41, "tuning table");
    CHECK(gsmrHzToArfcn(922.2e6) == 960 && std::fabs(gsmrArfcnToHz(960) - 922.2e6) < 1 && gsmrHzToArfcn(935.0e6) == 0 && gsmrHzToArfcn(957.4e6) == 112 &&
          gsmrHzToArfcn(925.2e6) == 975 && gsmrHzToArfcn(1.8e9) == -1 && gsmrHzToArfcn(1805.2e6) == 512 && gsmrHzToArfcn(1879.8e6) == 885 && gsmrArfcnToHz(600) == 1822.8e6, "ARFCN raster");
    CHECK(gsmrIsRailway(955) && gsmrIsRailway(973) && !gsmrIsRailway(974) && !gsmrIsRailway(50), "railway band");
    // scene 0: two cells at 5 Msps, 20 dB, 8 kHz tuning error
    {
        const GsmrTelemetry t = run(0, 20, 8000, 5e6, 2);
        printf("  scene 0: %s\n", gsmrSummary(t).c_str());
        for (int i = 0; i < 2; i++) {
            const GsmrCellCfg want = gsmrTestCell(0, i);
            const GsmrCell* c = find(t, want.arfcn);
            CHECK(c != nullptr, "cell ARFCN %d not found", want.arfcn);
            if (!c) continue;
            CHECK(c->bsic == ((want.ncc << 3) | want.bcc) && c->railway, "BSIC %d", c->bsic);
            CHECK(c->hasLai && c->mcc == 1 && c->mnc == 1 && c->lac == want.si.lai.lac, "LAI %d-%d %u", c->mcc, c->mnc, c->lac);
            CHECK(c->hasSi3 && c->cellId == want.si.cellId, "cell id %u", c->cellId);
            CHECK(c->hasSi1 && c->cellAlloc == want.cellAlloc && c->hasSi2 && c->neigh == want.neigh, "channel lists");
            CHECK(std::fabs(c->cfoHz - 8000) < 1500, "offset %.0f Hz", c->cfoHz);
            CHECK(c->t3212 == want.si.t3212 && c->rxLevAccessMin == want.si.rxLevAccessMin, "SI3 parameters");
        }
        CHECK(t.carriers.size() >= 2, "carriers %zu", t.carriers.size());
    }
    // scene 1 at the lower band edge (1.6 MHz below the centre), -8 kHz, 12 dB
    {
        const GsmrTelemetry t = run(1, 12, -8000, 5e6, 3);
        printf("  scene 1: %s\n", gsmrSummary(t).c_str());
        const GsmrCellCfg want = gsmrTestCell(1, 0);
        const GsmrCell* c = find(t, want.arfcn);
        CHECK(c && c->hasLai && c->lac == want.si.lai.lac && c->bsic == ((want.ncc << 3) | want.bcc), "cell at the band edge");
    }
    // a 2.4 Msps radio sees 2.4 MHz of the band: of scene 2 the cell at 923.0 MHz (ARFCN 964) is in it
    {
        const GsmrTelemetry t = run(2, 15, 2000, 2.4e6, 3);
        printf("  2.4 Msps: %s\n", gsmrSummary(t).c_str());
        const GsmrCellCfg want = gsmrTestCell(2, 1);
        const GsmrCell* c = find(t, want.arfcn);
        CHECK(c && c->hasSi3 && c->cellId == want.si.cellId && c->lac == want.si.lai.lac, "cell at 2.4 Msps");
    }
    // scene 2: three cells, the third 12 dB weaker; the two strong ones must be found, a wrong cell never
    {
        const GsmrTelemetry t = run(2, 18, 3000, 5e6, 2);
        printf("  scene 2: %s\n", gsmrSummary(t).c_str());
        for (int i = 0; i < 2; i++) {
            const GsmrCellCfg want = gsmrTestCell(2, i);
            const GsmrCell* c = find(t, want.arfcn);
            CHECK(c && c->hasSi3 && c->cellId == want.si.cellId, "cell %d of scene 2", i);
        }
        const GsmrCellCfg weak = gsmrTestCell(2, 2);
        if (const GsmrCell* c = find(t, weak.arfcn)) CHECK(!c->hasSi3 || c->cellId == weak.si.cellId, "weak cell decoded wrongly");
        for (const auto& c : t.cells) CHECK(c.arfcn == 957 || c.arfcn == 964 || c.arfcn == 971, "cell found at ARFCN %d that was not sent", c.arfcn);
    }
    // noise only: nothing found
    {
        SynthConfig sc;
        sc.mode = 41; sc.snrDb = 99; sc.modeOpt[0] = 1;
        GsmrReceiver rx;
        rx.configure(5e6);
        rx.setCenterHz(kGsmrTestCentreHz);
        std::vector<cf32> n(250000);
        uint32_t s = 1;
        for (int k = 0; k < 60; k++) {
            for (auto& v : n) { s = s * 1664525u + 1013904223u; const float a = ((s >> 8) & 0xFFFF) / 65536.f - 0.5f; s = s * 1664525u + 1013904223u; v = cf32(a * 0.2f, (((s >> 8) & 0xFFFF) / 65536.f - 0.5f) * 0.2f); }
            rx.feed(n.data(), n.size());
            while (rx.busy()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        GsmrTelemetry t;
        rx.telemetry(t, 0);
        CHECK(t.cells.empty(), "%zu cells found in noise", t.cells.size());
    }
    printf(fails ? "gsmr rx: %d FAILED\n" : "gsmr rx: all passed\n", fails);
    return fails ? 1 : 0;
}
