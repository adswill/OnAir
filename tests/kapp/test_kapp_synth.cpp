// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAPP through the engine on the synthetic source (setStandard(126)): the built-in test signal is the real transmitter's output (three text messages in turn) with a
// carrier offset of +1000 Hz and a little noise; the receiver must lock, decode and read the carrier offset back. Also: the transmit-mode and tuning tables name it,
// and a second start with the signal option set (modeOpt[0]) still decodes.
#include "dect2/engine/engine.h"
#include "dect2/kapp/kapp_tel.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

static bool run(int opt, double maxSec) {
    const ModeTuning mt = kappTuning();
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = 433e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 126;
    t.synth.modeOpt[0] = opt;
    FileOptions fo;
    e.setStandard(126);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 125, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    const auto& k = rx.kapp;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    double doneAt = -1;
    const auto t0 = std::chrono::steady_clock::now();
    while (true) {
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (el > maxSec) break;
        if (e.latestRx(rx, seq)) {
            seq = rx.seq; reports++;
            if (rx.standard != 125) wrong++;
            if (doneAt < 0 && (k.messages >= 3 && k.state == 2)) doneAt = el;
            if (doneAt >= 0 && el > doneAt + 2) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    printf("  %s: %d reports, %llu messages (%llu repeats), blocks ok %llu bad %llu, cfo %.0f Hz, profile %d, \"%s\"\n", kappSummary(k).c_str(), reports, (unsigned long long)k.messages, (unsigned long long)k.repeats, (unsigned long long)k.blocksOk, (unsigned long long)k.blocksBad, k.cfoHz, k.profile, k.packets.empty() ? "" : k.packets[0].preview.c_str());
    CHECK(reports > 5 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(doneAt > 0, "no decode within %.0f s", maxSec);
    CHECK(k.messages >= 3, "messages %llu", (unsigned long long)k.messages);
    CHECK(k.blocksOk > 3 && k.blocksBad * 10 <= k.blocksOk, "blocks ok %llu bad %llu", (unsigned long long)k.blocksOk, (unsigned long long)k.blocksBad);
    CHECK(std::fabs(k.cfoHz - 1000.0) < 200.0, "carrier offset read %.0f Hz, sent 1000", k.cfoHz);
    CHECK(k.callsign == "ONAIR", "callsign '%s'", k.callsign.c_str());
    CHECK(!k.packets.empty() && k.packets[0].preview.find("OnAir KAPP test signal") == 0, "message text");
    CHECK(k.profile == (opt > 0 ? opt - 1 : 2), "profile %d", k.profile);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return doneAt > 0;
}

int main() {
    const ModeTuning mt = kappTuning();
    CHECK(mt.stdMode == 126 && modeTuningById("kapp") && modeTuningById("kapp")->stdMode == 126, "tuning table");
    run(0, 90);
    run(1, 90);   // modeOpt[0] = 1: profile 0, BPSK
    return testSummary("kapp synth");
}
