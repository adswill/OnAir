// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sensitivity of the Smart CubeSat chains: for each modulation and framing the lowest carrier-to-noise (dB in 10 kHz) at which at least 2 of 3 trials
// decode at least 3 of 4 frames (2 of 4 for CW: the first message of a train is partly eaten by the keying detector's warm-up), 2 dB steps downward from 30 dB. Printed as a table (docs/CUBESAT-SMART.md quotes it). Only a coarse bound is asserted
// (every case must decode at 30 dB): the numbers are measurements of this receiver, not claims about a theory.
#include "dect2/cubesat/cubesat_chain.h"
#include "dect2/cubesat/cubesat_db.h"
#include "dect2/cubesat/cubesat_smartgen.h"
#include "dect2/cubesat/cubesat_telem.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::cubesat;
#include "../common/test_util.h"
#include "../common/test_long.h"

struct Case { const char* name; int mod, baud, framing; bool scramble; };

static int decoded(const Case& c, double snr, uint64_t seed) {
    SmartSynth s;
    s.mod = c.mod; s.baud = c.baud; s.framing = c.framing; s.scramble = c.scramble; s.rate = kChanRate; s.snrDb = snr; s.frames = 4; s.seed = seed;
    s.carrierHz = 300; s.clockPpm = 50;
    std::vector<SynthSent> sent;
    const std::vector<cf32> iq = smartSynth(s, &sent);
    std::unique_ptr<Chain> ch;
    switch (c.mod) {
    case ModFsk: ch = makeFskChain(c.baud); break;
    case ModAfsk: ch = makeAfskChain(); break;
    case ModBpsk: ch = makeBpskChain(c.baud); break;
    default: ch = makeCwChain(); break;
    }
    std::vector<ChainFrame> got;
    ch->sink = [&](ChainFrame& f) { got.push_back(f); };
    for (size_t i = 0; i < iq.size(); i += 4096) ch->process(iq.data() + i, std::min<size_t>(4096, iq.size() - i));
    int ok = 0;
    for (const SynthSent& e : sent)
        for (const ChainFrame& f : got)
            if (f.data == e.data || (!e.text.empty() && f.text.find(e.text) != std::string::npos)) { ok++; break; }
    return ok;
}

int main() {
    const Case cases[] = {
        {"AFSK 1200 AX.25", ModAfsk, 1200, Ax25Afsk1200, false},
        {"FSK 9600 G3RUH AX.25", ModFsk, 9600, Ax25G3ruh, true},
        {"FSK 4800 G3RUH AX.25", ModFsk, 4800, Ax25G3ruh, true},
        {"GMSK 9600 AX100 Mode 5", ModFsk, 9600, Ax100Mode5, true},
        {"BPSK 1200 AX.25", ModBpsk, 1200, Ax25G3ruh, false},
        {"BPSK 1200 AO-40 FEC", ModBpsk, 1200, kFramingAo40, true},
        {"CW 18 wpm", ModCw, 0, kFramingCw, true},
    };
    printf("%-26s %s\n", "case", "floor (dB in 10 kHz)");
    for (const Case& c : cases) {
        double floorDb = 99;
        int fails = 0;
        // by default 6 dB steps (a coarse floor; the 30 dB point the check needs is always run); the long run (ONAIR_TEST_LONG=1) steps by 2 dB for the table
        for (double snr = 30; snr >= -2 && fails < 2; snr -= testLongPick(6.0, 2.0)) {
            int good = 0;
            for (int t = 0; t < 3; t++) if (decoded(c, snr, 11 + (uint64_t)t) >= (c.mod == ModCw ? 2 : 3)) good++;
            if (good >= 2) { floorDb = snr; fails = 0; } else fails++;
        }
        printf("%-26s %.0f\n", c.name, floorDb);
        CHECK(floorDb <= 30, "%s does not decode at 30 dB", c.name);
    }
    return testSummary("cubesat sens");
}
