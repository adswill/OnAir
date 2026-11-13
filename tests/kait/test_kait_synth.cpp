// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAIT through the engine on the synthetic source (setStandard(133)): the built-in test signal is the real transmitter's output (the built-in test picture as a 10 s burst, then 2 s of silence) with a
// carrier offset of +500 Hz and a little noise; the receiver must lock, decode and read the carrier offset back. Also: the transmit-mode and tuning tables name it,
// and a second start with the signal option set (modeOpt[0]) still decodes.
#include "dect2/engine/engine.h"
#include "dect2/kait/kait_tel.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

static bool run(int opt, double maxSec) {
    const ModeTuning mt = kaitTuning();
    Engine e;
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = 433e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 133;
    t.synth.modeOpt[0] = opt;
    FileOptions fo;
    e.setStandard(133);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 132, "active standard %d", e.activeStandard());
    RxTelemetry rx;
    const auto& k = rx.kait;
    uint64_t seq = 0;
    int reports = 0, wrong = 0;
    double doneAt = -1;
    const auto t0 = std::chrono::steady_clock::now();
    while (true) {
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (el > maxSec) break;
        if (e.latestRx(rx, seq)) {
            seq = rx.seq; reports++;
            if (rx.standard != 132) wrong++;
            if (doneAt < 0 && (k.state == 4 && k.headerOk && k.nLayers > 0 && k.layersComplete == k.nLayers)) doneAt = el;
            if (doneAt >= 0 && el > doneAt + 1) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    printf("  %s: %d reports, bursts %llu decoded %llu, layers %d of %d, %zu bytes, cfo %.0f Hz, %dx%d \"%s\"\n", kaitSummary(k).c_str(), reports, (unsigned long long)k.bursts, (unsigned long long)k.burstsDecoded, k.layersComplete, k.nLayers, k.bytesGood, k.cfoHz, k.width, k.height, k.callsign.c_str());
    CHECK(reports > 5 && wrong == 0, "%d reports, %d wrong", reports, wrong);
    CHECK(doneAt > 0, "no decode within %.0f s", maxSec);
    CHECK(k.headerOk && k.callsign == "ONAIR", "header / callsign '%s'", k.callsign.c_str());
    CHECK(k.nLayers > 0 && k.layersComplete == k.nLayers, "layers %d of %d", k.layersComplete, k.nLayers);
    CHECK(k.image && !k.image->rgb.empty() && k.image->w == k.width && k.image->h == k.height, "the picture");
    CHECK(k.profile == (opt > 0 ? opt - 1 : 0), "profile %d", k.profile);
    CHECK(std::fabs(k.cfoHz - 500.0) < 150.0, "carrier offset read %.0f Hz, sent 500", k.cfoHz);
    CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
    e.stop();
    return doneAt > 0;
}

int main() {
    const ModeTuning mt = kaitTuning();
    CHECK(mt.stdMode == 133 && modeTuningById("kait") && modeTuningById("kait")->stdMode == 133, "tuning table");
    run(0, 100);
    run(2, 100);   // modeOpt[0] = 2: profile 1, robust
    return testSummary("kait synth");
}
