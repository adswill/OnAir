// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VOR / ILS through the engine (setStandard(32)): the synthetic source plays a Doppler VOR on radial 123.4, the VOR entry measures it
// and reports through latestRx(); then the ILS entry on a localizer channel reads the DDM; a retune keeps the report numbers growing.
#include "dect2/engine/engine.h"
#include "dect2/vor/vor_rx.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

static RxTelemetry runFor(Engine& e, double secs, int& reports, int& wrong) {
    RxTelemetry rx;
    uint64_t seq = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < secs) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 31) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return rx;
}

int main() {
    const ModeTuning mt = vorTuning();
    const SubMode* vor = subModeById("vor");
    const SubMode* ils = subModeById("ils");
    CHECK(mt.stdMode == 32 && modeTuningById("vorils") && vor && ils && vor->stdMode == 32 && ils->stdMode == 32, "tables");
    {
        Engine e;
        DeviceInfo dev;
        TuneSettings t;
        t.centerHz = vor->defMhz * 1e6;
        t.bandwidthMhz = mt.bandwidthMhz;
        t.sampleRate = mt.sampleRate;
        t.synth.mode = 32;
        applySubModeSynth(t.synth, *vor);
        t.synth.modeOpt[0] = 1;          // Doppler VOR
        t.synth.modeVal[0] = 123.4;
        t.synth.snrDb = 30;
        t.synth.cfoHz = 1500;
        FileOptions fo;
        e.vor().setSilent(true);
        e.setStandard(32);
        applySubMode(e, vor);
        CHECK(e.start(dev, t, fo), "engine start");
        CHECK(e.activeStandard() == 31, "active standard %d", e.activeStandard());
        int reports = 0, wrong = 0;
        RxTelemetry rx = runFor(e, 9.0, reports, wrong);
        const VorTelemetry& v = rx.vor;
        printf("  %d reports: %s, carrier %+.0f Hz, last ident '%s'\n", reports, vorSummary(v).c_str(), v.cfoHz, v.identLast.c_str());
        CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
        CHECK(rx.rateOk && v.function == NavFunction::Vor, "rate / function %d", (int)v.function);
        CHECK(v.vorValid && std::fabs(std::remainder(v.radialDeg - 123.4, 360.0)) < 1.0, "radial %.2f", v.radialDeg);
        CHECK(std::fabs(v.cfoHz - 1500) < 30, "carrier %.0f", v.cfoHz);
        CHECK(v.identLast == "DXB", "ident '%s'", v.identLast.c_str());
        CHECK(std::fabs(v.dialHz - t.centerHz) < 1, "dial %.0f", v.dialHz);
        CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
        // the ILS entry on a localizer channel: the same receiver measures the DDM (the test signal follows the entry)
        const uint64_t before = rx.seq;
        e.stop();
        t.centerHz = ils->defMhz * 1e6;
        applySubModeSynth(t.synth, *ils);
        t.synth.modeVal[0] = -0.05;
        applySubMode(e, ils);
        CHECK(e.start(dev, t, fo), "engine restart");
        rx = runFor(e, 3.0, reports, wrong);
        printf("  ILS: %s\n", vorSummary(rx.vor).c_str());
        CHECK(rx.vor.function == NavFunction::Localizer && rx.vor.ilsValid && std::fabs(rx.vor.ddm + 0.05) < 0.005, "localizer: function %d ddm %.4f", (int)rx.vor.function, rx.vor.ddm);
        TuneSettings t2 = t; t2.centerHz = 332.0e6;
        e.retuneReset(t2);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        e.latestRx(rx, 0);
        CHECK(rx.seq > before && rx.vor.function == NavFunction::Glideslope, "after retune to 332 MHz: seq %llu function %d", (unsigned long long)rx.seq, (int)rx.vor.function);
        e.stop();
    }
    printf(fails ? "vor engine: %d FAILED\n" : "vor engine: all passed\n", fails);
    return fails ? 1 : 0;
}
