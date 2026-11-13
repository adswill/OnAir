// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FLARM through the engine (setStandard(68)): the synthetic source plays six made-up aircraft; the receiver reports through latestRx().
#include "dect2/engine/engine.h"
#include "dect2/flarm/flarm_gen.h"
#include "dect2/flarm/flarm_rx.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = flarmTuning();
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 68;
    t.synth.snrDb = 25;
    t.synth.modeVal[0] = 54.70; t.synth.modeVal[1] = 25.30;
    FileOptions fo;
    e.setStandard(68);
    e.flarm().setReference(54.70, 25.30);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 67, "active standard %d", e.activeStandard());
    e.flarm().setReference(54.70, 25.30);
    RxTelemetry rx;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 9.0) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; reports++; if (rx.standard != 67) wrong++; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const FlarmTelemetry& f = rx.flarm;
    printf("  %d reports: %s, %llu ok, %llu bad, privacy %llu, new %llu\n", reports, flarmSummary(f).c_str(), (unsigned long long)f.blocksOk, (unsigned long long)f.blocksBad,
           (unsigned long long)f.privacyFlagged, (unsigned long long)f.newProtocol);
    CHECK(reports > 10 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(rx.rateOk, "rate");
    CHECK(f.aircraftCount == 3 && f.withPosition == 3, "%u aircraft, %u with a position", f.aircraftCount, f.withPosition);
    CHECK(f.privacyFlagged == 2 && f.newProtocol > 0, "privacy %llu, new protocol %llu", (unsigned long long)f.privacyFlagged, (unsigned long long)f.newProtocol);
    for (const auto& a : f.aircraft) CHECK(a.id != 0xDD9999 && a.id != 0xDD8888, "flagged aircraft listed");
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    printf(fails ? "flarm engine: %d FAILED\n" : "flarm engine: all passed\n", fails);
    return fails ? 1 : 0;
}
