// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode A / C through the engine (setStandard(144)): the synthetic source plays the airspace with replies and the receiver reports codes and the
// Mode S aircraft through latestRx(); some codes are matched to aircraft.
#include "dect2/engine/engine.h"
#include "dect2/modeac/modeac_live.h"
#include "dect2/modeac/modeac_tel.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = modeacTuning();
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6; t.bandwidthMhz = mt.bandwidthMhz; t.sampleRate = mt.sampleRate;
    t.synth.mode = 144; t.synth.snrDb = 30; t.synth.modeVal[0] = 4;
    FileOptions fo;
    e.setStandard(144);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 143, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    bool got = false;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 60) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; if (rx.standard == 143 && rx.modeac.codeCount >= 6 && rx.modeac.matched >= 1) { got = true; break; } }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    printf("  %s\n", modeAcSummary(rx.modeac).c_str());
    CHECK(got, "codes %u matched %u after 60 s", rx.modeac.codeCount, rx.modeac.matched);
    CHECK(rx.modeac.adsb.aircraftCount >= 3, "Mode S aircraft %u", rx.modeac.adsb.aircraftCount);
    CHECK(rx.rateOk && rx.dataValid, "rate %d valid %d", (int)rx.rateOk, (int)rx.dataValid);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    for (const auto& c : rx.modeac.codes) if (c.icao) { CHECK(c.kind != 0, "a matched code with no type"); break; }
    e.stop();
    return testSummary("modeac engine");
}
