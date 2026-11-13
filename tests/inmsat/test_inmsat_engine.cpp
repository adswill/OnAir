// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Inmarsat all-carriers mode through the engine (setStandard(210)): the synthetic source plays several carriers in one 6 Msps capture
// (Aero test signal x2, Inmarsat-C with an extra channel, a wide carrier), the receiver reports through latestRx(). Synthetic only.
#include "dect2/inmsat/inmsat_rx.h"
#include "dect2/inmsat/inmsat_tel.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = inmsatTuning();
    CHECK(mt.stdMode == 210 && modeTuningById("inmsat") && modeTuningById("inmsat")->stdMode == 210 && modeTuning(210), "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = 6e6;
    t.synth.mode = 210;
    FileOptions fo;
    e.setStandard(210);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 209, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 50.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 209) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const InmsatTelemetry& s = rx.inmsat;
    printf("  %d reports: %s\n", reports, inmsatSummary(s).c_str());
    for (const auto& c : s.carriers) printf("    %.4f MHz  %-10s %5d  state %d  %s\n", c.freqHz / 1e6, inmsat::carrierTypeName(c.type), c.bitRate, c.state, c.parked ? "parked" : "");
    CHECK(reports > 20 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    int dec = 0;
    for (const auto& c : s.carriers) dec += c.state >= 2;
    CHECK(dec >= 5, "carriers decoding: %d", dec);
    CHECK(s.messagesTotal >= 4 && !s.aircraft.empty(), "%llu messages, %zu aircraft", (unsigned long long)s.messagesTotal, s.aircraft.size());
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return testSummary("inmsat engine");
}
