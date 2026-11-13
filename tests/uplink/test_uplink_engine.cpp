// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode S uplink through the engine, as the GUI uses it: the synthetic source plays the test scene (SynthConfig::mode = 45) in real time and
// the receiver must find the interrogators (II 3 with its 4.8 s rotation, SI 17, Mode A/C), the aircraft and the ACAS coordination.
#include "dect2/engine/engine.h"
#include "dect2/engine/modes.h"
#include "dect2/uplink/uplink_rx.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    CHECK(modeTuningById("uplink") && modeTuningById("uplink")->stdMode == 45, "tuning table");
    const ModeTuning mt = uplinkTuning();
    Engine e;
    DeviceInfo dev;
    dev.kind = DeviceInfo::Synthetic;
    dev.name = "synthetic";
    TuneSettings tune;
    tune.sampleRate = mt.sampleRate;
    tune.bandwidthMhz = mt.bandwidthMhz;
    tune.centerHz = mt.defMhz * 1e6;
    tune.synth.mode = 45;
    tune.synth.snrDb = 30;
    tune.synth.cfoHz = 60e3;
    e.setStandard(45);
    CHECK(e.start(dev, tune, FileOptions()), "engine start");
    CHECK(e.activeStandard() == 44, "active standard %d", e.activeStandard());
    RxTelemetry t;
    uint64_t seq = 0;
    bool done = false;
    for (int i = 0; i < 64 && !done; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (!e.latestRx(t, seq)) continue;
        seq = t.seq;
        if (t.standard != 44) continue;
        for (const auto& x : t.uplink.interrogators) done |= x.ii == 3 && x.periodSec > 0;
    }
    e.latestRx(t, 0);
    const UplinkTelemetry& u = t.uplink;
    printf("  %s\n", uplinkSummary(u).c_str());
    bool ii3 = false, si17 = false, ac = false;
    for (const auto& x : u.interrogators) {
        ii3 |= x.ii == 3 && std::fabs(x.periodSec - 4.8) < 0.15;
        si17 |= x.si == 17;
        ac |= x.kind == 1 && x.modeA > 0 && x.modeC > 0;
    }
    CHECK(t.standard == 44 && t.rateOk, "standard %d", t.standard);
    CHECK(ii3, "II 3 with a 4.8 s rotation");
    CHECK(si17, "SI 17");
    CHECK(ac, "Mode A/C");
    CHECK(u.uf[0] > 0 && u.uf[11] > 0 && u.modeS > 20, "Mode S formats");
    CHECK(std::fabs(u.cfoHz - 60e3) < 25e3, "offset %.0f Hz", u.cfoHz);
    CHECK(!modeSummary(t).empty(), "summary");
    e.stop();
    printf(fails ? "uplink engine: %d FAILED\n" : "uplink engine: all passed\n", fails);
    return fails ? 1 : 0;
}
