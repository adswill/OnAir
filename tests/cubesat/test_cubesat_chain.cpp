// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Smart CubeSat demodulation chains on synthetic bursts at 96 kHz: every modulation and framing, with a carrier offset, a Doppler ramp, a
// transmitter clock error and noise. Each case counts the frames that validated (and match what was sent) against the frames sent.
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

struct Case {
    const char* name;
    SmartSynth s;
    int minOk;               // frames that must come out right
    int chainMod, chainBaud; // the chain to run (the hypothesis), baud 0 for CW / AFSK
};

static int run(const Case& c, bool verbose) {
    SmartSynth s = c.s;
    s.rate = kChanRate;
    std::vector<SynthSent> sent;
    std::vector<cf32> iq = smartSynth(s, &sent);
    std::unique_ptr<Chain> ch;
    switch (c.chainMod) {
    case ModFsk: ch = makeFskChain(c.chainBaud); break;
    case ModAfsk: ch = makeAfskChain(); break;
    case ModBpsk: ch = makeBpskChain(c.chainBaud); break;
    default: ch = makeCwChain(); break;
    }
    std::vector<ChainFrame> got;
    ch->sink = [&](ChainFrame& f) { got.push_back(f); };
    for (size_t i = 0; i < iq.size(); i += 4096) ch->process(iq.data() + i, std::min<size_t>(4096, iq.size() - i));
    int ok = 0;
    for (const SynthSent& e : sent) {
        for (const ChainFrame& f : got) {
            bool same = f.data == e.data;
            if (!same && !e.text.empty()) same = f.text.find(e.text) != std::string::npos || (s.mod == ModCw && f.text == e.text);
            if (same) { ok++; break; }
        }
    }
    CHECK(ok >= c.minOk, "%s: %d of %zu frames right (need %d), %zu frames out", c.name, ok, sent.size(), c.minOk, got.size());
    if (ok < c.minOk) for (const ChainFrame& f : got) if (!f.text.empty()) printf("    got text: \"%s\" (sent \"%s\")\n", f.text.c_str(), sent.empty() ? "" : sent[0].text.c_str());
    if (verbose || ok < c.minOk) {
        std::vector<TrialStats> st;
        ch->stats(st);
        for (const TrialStats& t : st) if (t.valid || t.heard) printf("    %s: valid %llu heard %llu\n", t.label.c_str(), (unsigned long long)t.valid, (unsigned long long)t.heard);
    }
    printf("%-44s %d/%zu\n", c.name, ok, sent.size());
    return ok;
}

static SmartSynth mk(int mod, int baud, int framing, double snr, double off = 0, double dopp = 0, double rate = 0, double ppm = 0) {
    SmartSynth s;
    s.mod = mod; s.baud = baud; s.framing = framing; s.snrDb = snr; s.carrierHz = off; s.dopplerHz = dopp; s.dopplerRateHzS = rate; s.clockPpm = ppm;
    s.frames = 4;
    return s;
}

int main() {
    std::vector<Case> cases;
    auto add = [&](const char* n, SmartSynth s, int minOk, int cm, int cb) { cases.push_back({n, s, minOk, cm, cb}); };
    // FSK / GMSK
    add("FSK 9600 G3RUH AX.25 clean", mk(ModFsk, 9600, Ax25G3ruh, 40), 4, ModFsk, 9600);
    add("FSK 9600 G3RUH, offset +800 Hz", mk(ModFsk, 9600, Ax25G3ruh, 40, 800), 4, ModFsk, 9600);
    add("FSK 9600 G3RUH, offset -900, Doppler ramp", mk(ModFsk, 9600, Ax25G3ruh, 40, -900, 300, -40), 4, ModFsk, 9600);
    add("FSK 9600 G3RUH, 300 ppm clock", mk(ModFsk, 9600, Ax25G3ruh, 40, 0, 0, 0, 300), 4, ModFsk, 9600);
    add("FSK 9600 G3RUH, 20 dB", mk(ModFsk, 9600, Ax25G3ruh, 28), 3, ModFsk, 9600);
    add("FSK 4800 G3RUH", mk(ModFsk, 4800, Ax25G3ruh, 40), 4, ModFsk, 4800);
    add("FSK 2400 plain NRZI AX.25", [] { SmartSynth s = mk(ModFsk, 2400, Ax25G3ruh, 40); s.scramble = false; return s; }(), 4, ModFsk, 2400);
    add("FSK 1200 plain NRZI AX.25", [] { SmartSynth s = mk(ModFsk, 1200, Ax25G3ruh, 40); s.scramble = false; return s; }(), 3, ModFsk, 1200);
    add("FSK 19200 G3RUH", mk(ModFsk, 19200, Ax25G3ruh, 40), 4, ModFsk, 19200);
    add("FSK 9600 AX100 Mode 5", mk(ModFsk, 9600, Ax100Mode5, 40), 4, ModFsk, 9600);
    add("FSK 4800 AX100 Mode 5, offset 600", mk(ModFsk, 4800, Ax100Mode5, 40, 600), 4, ModFsk, 4800);
    add("FSK 9600 AX100 Mode 6", mk(ModFsk, 9600, Ax100Mode6, 40), 4, ModFsk, 9600);
    add("FSK 9600 CCSDS RS", mk(ModFsk, 9600, CcsdsRs, 40), 4, ModFsk, 9600);
    add("FSK 1200 ASCII text", mk(ModFsk, 1200, kFramingText, 40), 3, ModFsk, 1200);
    // 3.9 framings
    add("FSK 9600 U482C (RS, conv on every other)", mk(ModFsk, 9600, U482c, 40, 500), 4, ModFsk, 9600);
    add("FSK 9600 CCSDS concatenated, offset -700", mk(ModFsk, 9600, CcsdsConcat, 40, -700), 4, ModFsk, 9600);
    add("FSK 2400 USP, 200 ppm", mk(ModFsk, 2400, Usp, 40, 0, 0, 0, 200), 4, ModFsk, 2400);
    add("FSK 4800 NGHam, Doppler ramp", mk(ModFsk, 4800, Ngham, 40, 400, 300, -40), 4, ModFsk, 4800);
    add("FSK 4800 Mobitex-NX, 22 dB", mk(ModFsk, 4800, Mobitex, 30), 3, ModFsk, 4800);
    // AFSK
    add("AFSK 1200 AX.25", [] { SmartSynth s = mk(ModAfsk, 1200, Ax25Afsk1200, 40); s.scramble = false; return s; }(), 4, ModAfsk, 0);
    add("AFSK 1200 AX.25, offset +1500, 200 ppm", [] { SmartSynth s = mk(ModAfsk, 1200, Ax25Afsk1200, 40, 1500, 0, 0, 200); s.scramble = false; return s; }(), 4, ModAfsk, 0);
    // BPSK
    add("BPSK 1200 AX.25 NRZI", [] { SmartSynth s = mk(ModBpsk, 1200, Ax25G3ruh, 40); s.scramble = false; return s; }(), 4, ModBpsk, 1200);
    add("BPSK 1200 AX.25, offset +700", [] { SmartSynth s = mk(ModBpsk, 1200, Ax25G3ruh, 40, 700); s.scramble = false; return s; }(), 4, ModBpsk, 1200);
    add("BPSK 9600 G3RUH AX.25", mk(ModBpsk, 9600, Ax25G3ruh, 40), 4, ModBpsk, 9600);
    add("BPSK 9600 CCSDS concatenated", mk(ModBpsk, 9600, CcsdsConcat, 40, 300), 4, ModBpsk, 9600);
    add("BPSK 1200 AO-40 FEC (FUNcube)", mk(ModBpsk, 1200, kFramingAo40, 40), 4, ModBpsk, 1200);
    add("BPSK 1200 AO-40 FEC, offset -600, 150 ppm", mk(ModBpsk, 1200, kFramingAo40, 40, -600, 0, 0, 150), 4, ModBpsk, 1200);
    add("BPSK 1200 AO-40 FEC, Doppler ramp", mk(ModBpsk, 1200, kFramingAo40, 40, 300, 500, -60), 4, ModBpsk, 1200);
    // CW
    add("CW 18 wpm", [] { SmartSynth s = mk(ModCw, 0, kFramingCw, 40); s.frames = 3; return s; }(), 3, ModCw, 0);
    add("CW 12 wpm, offset +300 Hz", [] { SmartSynth s = mk(ModCw, 0, kFramingCw, 40, 300); s.frames = 3; s.wpm = 12; return s; }(), 3, ModCw, 0);
    add("CW 25 wpm, 30 dB", [] { SmartSynth s = mk(ModCw, 0, kFramingCw, 30); s.frames = 3; s.wpm = 25; return s; }(), 3, ModCw, 0);
    for (const Case& c : cases) run(c, false);
    // Morse decoder alone
    {
        const auto r = morseRuns("DE ES5E 123", 15);
        double u = 0;
        CHECK(morseDecode(r, &u) == "DE ES5E 123" && u > 0.07 && u < 0.09, "Morse round trip (unit %.3f)", u);
        CHECK(morseDecode({{true, 0.1}, {false, 0.1}, {true, 0.1}}, nullptr).empty(), "too short is no Morse");
    }
    return testSummary("cubesat chain");
}
