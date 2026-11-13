// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The HF utility receiver through the channel front end: the test signal of every mode, at the wide input rate and 20 kHz below the radio, is read back.
#include "dect2/hfutil/hfutil_rx.h"
#include <cstdio>
#include <vector>

using namespace dect2;

int main() {
    int fails = 0;
    for (int k = 0; k < kHfuKinds; k++) {
        const double rate = 1000000;
        SynthConfig cfg;
        cfg.snrDb = 30;
        auto synth = makeHfUtilSynth(k, cfg, rate);
        HfUtilReceiver rx(k);
        rx.configure(rate);
        rx.setSignalOffset(-20000);
        std::vector<cf32> buf(50000);
        // two passes of the looping test signal, at most 90 s
        HfUtilTelemetry t;
        uint64_t seq = 0;
        double secs = k == 0 || k == 1 ? 40 : 45;
        for (double s = 0; s < secs; s += 0.05) { synth->generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
        rx.telemetry(t, seq);
        const bool ok = t.text.find("UTILITY") != std::string::npos || (k == 7 && !t.events.empty());   // the state is back to searching after an end of message
        if (!ok) { fails++; std::printf("FAIL kind %d (%s): state %d mode '%s', text '%s', %zu events\n", k, hfuName(k), t.state, t.mode.c_str(), t.text.substr(0, 100).c_str(), t.events.size()); }
        else std::printf("%s: %s\n", hfuName(k), t.mode.c_str());
    }
    if (fails) std::printf("%d FAILED\n", fails); else std::printf("OK\n");
    return fails ? 1 : 0;
}
