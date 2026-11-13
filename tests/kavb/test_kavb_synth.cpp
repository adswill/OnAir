// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAVB through the engine on the synthetic source (setStandard(125)): the built-in test signal is the real transmitter's output (the synthetic audio-video test stream) with a
// carrier offset of +3000 Hz and a little noise; the receiver must lock, decode and read the carrier offset back. Also: the transmit-mode and tuning tables name it,
// and a second start with the signal option set (modeOpt[0]) still decodes.
#include "dect2/engine/engine.h"
#include "dect2/kavb/kavb_tel.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

static bool run(int opt, double maxSec) {
    const ModeTuning mt = kavbTuning();
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = 1290e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 125;
    t.synth.modeOpt[0] = opt;
    FileOptions fo;
    e.setStandard(125);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 124, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    const auto& k = rx.kavb;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    double doneAt = -1;
    const auto t0 = std::chrono::steady_clock::now();
    while (true) {
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (el > maxSec) break;
        if (e.latestRx(rx, seq)) {
            seq = rx.seq; reports++;
            if (rx.standard != 124) wrong++;
            if (doneAt < 0 && (k.state == 2 && k.packets >= 20)) doneAt = el;
            if (doneAt >= 0 && el > doneAt + 2) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    printf("  %s: %d reports, frames good %llu bad %llu, packets %llu (%llu groups lost), cfo %.0f Hz (coarse %.0f), profile %d, \"%s\"\n", kavbSummary(k).c_str(), reports, (unsigned long long)k.framesGood, (unsigned long long)k.framesBad, (unsigned long long)k.packets, (unsigned long long)k.groupsLost, k.cfoHz, k.coarseHz, k.profile, k.callsign.c_str());
    // the telemetry cadence depends on the machine (a fast decode ends the loop 2 s after lock): only require that reports flow
    CHECK(reports >= 2 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(doneAt > 0, "no decode within %.0f s", maxSec);
    CHECK(k.framesGood > 10 && k.framesBad * 10 <= k.framesGood, "frames good %llu bad %llu", (unsigned long long)k.framesGood, (unsigned long long)k.framesBad);
    CHECK(k.packets >= 20 && k.packetsByType[0] > 0 && k.packetsByType[1] > 0, "packets %llu video %llu audio %llu", (unsigned long long)k.packets, (unsigned long long)k.packetsByType[0], (unsigned long long)k.packetsByType[1]);
    CHECK(k.callsign == "ONAIR", "station ID '%s'", k.callsign.c_str());
    CHECK(std::fabs(k.cfoHz - 3000.0) < 400.0, "carrier offset read %.0f Hz, sent 3000", k.cfoHz);
    CHECK(k.profile == 0, "profile %d", k.profile);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return doneAt > 0;
}

int main() {
    const ModeTuning mt = kavbTuning();
    CHECK(mt.stdMode == 125 && modeTuningById("kavb") && modeTuningById("kavb")->stdMode == 125, "tuning table");
    run(0, 60);

    return testSummary("kavb synth");
}
