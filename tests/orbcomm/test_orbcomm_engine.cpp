// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Orbcomm through the engine, as the GUI uses it: the synthetic source plays the test signal (SynthConfig::mode = 46) in real time with
// two downlinks and the +/-4 kHz Doppler ramp, the receiver runs on the engine's thread, and both satellites must be decoded with
// their positions, absolute frequencies on the 137 MHz raster and the Doppler.
#include "dect2/engine/engine.h"
#include "dect2/engine/modes.h"
#include "dect2/orbcomm/orbcomm_rx.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    const ModeTuning* mt = modeTuningById("orbcomm");
    CHECK(mt && mt->stdMode == 46, "tuning table");
    if (!mt) return 1;
    int np = 0;
    const OrbcommPreset* pr = orbcommPresets(np);
    CHECK(np >= 6 && std::fabs(pr[0].mhz - 137.25) < 1e-9, "presets");
    Engine e;
    DeviceInfo dev;
    dev.kind = DeviceInfo::Synthetic;
    dev.name = "synthetic";
    TuneSettings tune;
    tune.sampleRate = mt->sampleRate;
    tune.bandwidthMhz = mt->bandwidthMhz;
    tune.centerHz = mt->defMhz * 1e6;
    tune.synth.mode = 46;
    tune.synth.snrDb = 16;
    tune.synth.modeOpt[1] = 1;      // Doppler ramp
    tune.synth.modeVal[0] = 20;     // 20 s per leg
    tune.synth.modeVal[1] = 80;     // clock ppm
    FileOptions fo;
    e.setStandard(46);
    CHECK(e.start(dev, tune, fo), "engine start");
    CHECK(e.activeStandard() == 45, "active standard %d", e.activeStandard());
    RxTelemetry t;
    uint64_t seq = 0;
    bool both = false;
    for (int i = 0; i < 60 && !both; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (e.latestRx(t, seq)) {
            seq = t.seq;
            int withPos = 0;
            for (const auto& s : t.orbcomm.sats) withPos += s.hasPos && s.packets > 20;
            both = t.standard == 45 && withPos == 2;
        }
    }
    e.latestRx(t, 0);
    const OrbcommTelemetry& o = t.orbcomm;
    printf("  %s\n", orbcommSummary(o).c_str());
    CHECK(both, "two satellites with positions");
    CHECK(t.rateOk && o.blocksOk > 40, "%llu packets", (unsigned long long)o.blocksOk);
    for (const auto& s : o.sats) {
        CHECK(s.id == 23 || s.id == 41, "satellite id %d", s.id);
        const double want = s.id == 23 ? 137.25e6 : 137.30e6;
        CHECK(std::fabs(s.nominalHz - want) < 1, "sat %d on %.4f MHz", s.id, s.nominalHz / 1e6);
        CHECK(std::fabs(s.dopplerHz) <= 4100, "sat %d Doppler %.0f", s.id, s.dopplerHz);
        CHECK(s.altKm > 700 && s.altKm < 850, "sat %d height %.0f km", s.id, s.altKm);
    }
    CHECK(!modeSummary(t).empty(), "summary");
    e.stop();
    printf(fails ? "orbcomm engine: %d FAILED\n" : "orbcomm engine: all passed\n", fails);
    return fails ? 1 : 0;
}
