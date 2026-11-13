// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAFT through the engine on the synthetic source (setStandard(127)): the built-in test signal is the real transmitter's output (a small demo text file as one session, sent again and again) with a
// carrier offset of +1500 Hz and a little noise; the receiver must lock, decode and read the carrier offset back. Also: the transmit-mode and tuning tables name it,
// and a second start with the signal option set (modeOpt[0]) still decodes.
#include "dect2/engine/engine.h"
#include "dect2/kaft/kaft_tel.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

static bool run(int opt, double maxSec) {
    const ModeTuning mt = kaftTuning();
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = 1290e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 127;
    t.synth.modeOpt[0] = opt;
    FileOptions fo;
    e.setStandard(127);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 126, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    const auto& k = rx.kaft;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    double doneAt = -1;
    const auto t0 = std::chrono::steady_clock::now();
    while (true) {
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (el > maxSec) break;
        if (e.latestRx(rx, seq)) {
            seq = rx.seq; reports++;
            if (rx.standard != 126) wrong++;
            if (doneAt < 0 && (k.files.size() >= 1)) doneAt = el;
            if (doneAt >= 0 && el > doneAt + 2) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    printf("  %s: %d reports, files %zu \"%s\" %llu bytes, session state %d, blocks ok %llu bad %llu, cfo %.0f Hz, profile %d width %d\n", kaftSummary(k).c_str(), reports, k.files.size(), k.files.empty() ? "" : k.files[0].name.c_str(), (unsigned long long)(k.files.empty() ? 0 : k.files[0].bytes), k.session, (unsigned long long)k.blocksOk, (unsigned long long)k.blocksBad, k.cfoHz, k.profile, k.width);
    CHECK(reports > 5 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(doneAt > 0, "no decode within %.0f s", maxSec);
    CHECK(k.files.size() >= 1 && k.files[0].bytes > 50, "a file with a good hash arrived");
    CHECK(k.session == 3, "session state %d (3 = complete and verified)", k.session);
    CHECK(k.blocksOk >= 1 && k.blocksBad * 10 <= k.blocksOk, "blocks ok %llu bad %llu", (unsigned long long)k.blocksOk, (unsigned long long)k.blocksBad);
    CHECK(std::fabs(k.cfoHz - 1500.0) < 300.0, "carrier offset read %.0f Hz, sent 1500", k.cfoHz);
    CHECK(k.peer == "ONAIR" || k.callsign == "ONAIR", "callsign '%s' / '%s'", k.peer.c_str(), k.callsign.c_str());
    CHECK(k.width == 2, "width %d", k.width);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return doneAt > 0;
}

int main() {
    const ModeTuning mt = kaftTuning();
    CHECK(mt.stdMode == 127 && modeTuningById("kaft") && modeTuningById("kaft")->stdMode == 127, "tuning table");
    run(0, 120);
    run(1, 150);   // modeOpt[0] = 1: profile 0, BPSK
    return testSummary("kaft synth");
}
