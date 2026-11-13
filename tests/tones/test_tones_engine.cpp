// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Tone decoders through the engine (setStandard(91)): the synthetic source plays the 40 s demonstration (CTCSS 123.0 Hz, DCS D023N, DTMF 159#, ...);
// the receiver reports through latestRx(). No sound.
#include "dect2/engine/engine.h"
#include "dect2/tones/tones_rx.h"
#include "dect2/tones/tones_tel.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const ModeTuning mt = tonesTuning();
    CHECK(mt.stdMode == 91 && modeTuningById("tones") && modeTuning(91) && modeTuning(91)->tuneOffsetHz == 50000, "tuning table");
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6; t.bandwidthMhz = mt.bandwidthMhz; t.sampleRate = mt.sampleRate;
    t.synth.mode = 91; t.synth.snrDb = 30;
    FileOptions fo;
    e.setStandard(91);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 90, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    uint64_t seq = 0;
    bool ct = false, dcs = false, dtmf = false;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 40.0 && !(ct && dcs && dtmf)) {
        if (e.latestRx(rx, seq)) {
            seq = rx.seq;
            if (rx.standard == 90)
                for (const auto& ev : rx.tones.events) {
                    if (ev.kind == TkCtcss && ev.text == "123.0 Hz") ct = true;
                    if (ev.kind == TkDcs && ev.text == "D023N") dcs = true;
                    if (ev.kind == TkDtmf && ev.text == "159#") dtmf = true;
                }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    printf("  %s\n", tonesSummary(rx.tones).c_str());
    CHECK(ct && dcs && dtmf, "CTCSS %d DCS %d DTMF %d", (int)ct, (int)dcs, (int)dtmf);
    CHECK(rx.rateOk, "rate");
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return testSummary("tones engine");
}
