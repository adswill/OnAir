// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// EBICab / Eurobalise receiver on the generator's signals: bursts of repeated telegrams (long and short) starting at a random place, clean, then
// the impairments one by one: carrier offsets of +-40 kHz, the swapped frequencies, a data rate off by +-2 % (the standard allows 2.5 %), timing jitter,
// a carrier drift, noise, other sample rates; bursts of random bits must give no telegram but must be listed; noise alone gives nothing.
#include "dect2/ebicab/ebicab_gen.h"
#include "dect2/ebicab/ebicab_rx.h"
#include "dect2/ebicab/ebicab_tel.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static EbicabTelemetry run(EbicabGenConfig c, double rate, double sec) {
    c.rate = rate;
    c.dialOffsetHz = -1.0e6;
    EbicabGenerator g(c);
    EbicabReceiver r;
    r.configure(rate);
    r.setSignalOffset(-1.0e6);
    std::vector<cf32> buf(50000);
    const size_t tot = (size_t)(sec * rate);
    for (size_t i = 0; i < tot; i += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
    r.flush();
    EbicabTelemetry t;
    r.telemetry(t, 0);
    return t;
}

static EbicabGenConfig base() {
    EbicabGenConfig c;
    c.telegrams.push_back(ebicabTestTelegram(true, 1201, 0));
    c.telegrams.push_back(ebicabTestTelegram(true, 1201, 1));
    c.telegrams.push_back(ebicabTestTelegram(false, 1202, 0));
    return c;
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const double kRate = 10e6;
    {   // clean
        const EbicabTelemetry t = run(base(), kRate, 0.45);
        printf("  clean: %llu telegrams (%llu long, %llu short), %llu bursts, %zu balises, rate %.1f kbit/s\n", (unsigned long long)t.blocksOk, (unsigned long long)t.longTelegrams,
               (unsigned long long)t.shortTelegrams, (unsigned long long)t.bursts, t.balises.size(), t.bitRate / 1e3);
        CHECK(t.longTelegrams >= 4 && t.shortTelegrams >= 2, "telegrams %llu long %llu short", (unsigned long long)t.longTelegrams, (unsigned long long)t.shortTelegrams);
        CHECK(t.balises.size() == 3, "balises %zu", t.balises.size());
        bool a = false, b = false, c = false;
        for (const auto& x : t.balises) { if (x.nidC == 466 && x.nidBg == 1201 && x.nPig == 0) a = true; if (x.nidBg == 1201 && x.nPig == 1) b = true; if (x.nidBg == 1202) c = true; }
        CHECK(a && b && c, "balise identities");
        CHECK(std::fabs(t.bitRate - 564480) < 8000, "bit rate %.0f", t.bitRate);
        CHECK(t.blocksBad <= 1, "bursts without telegram %llu", (unsigned long long)t.blocksBad);
    }
    struct Case { const char* name; double cfo, drift, ppm, ebn0, jitter; bool inv; double rate; };
    const Case cs[] = {
        {"carrier +40 kHz", 40e3, 0, 0, 99, 0, false, 10e6}, {"carrier -40 kHz", -40e3, 0, 0, 99, 0, false, 10e6}, {"swapped frequencies", 0, 0, 0, 99, 0, true, 10e6},
        {"data rate +2 %", 0, 0, 20000, 99, 0, false, 10e6}, {"data rate -2 %", 0, 0, -20000, 99, 0, false, 10e6}, {"jitter 100 ns", 0, 0, 0, 99, 100, false, 10e6},
        {"drift 20 kHz/s", 0, 20e3, 0, 99, 0, false, 10e6}, {"noise Eb/N0 14 dB", 0, 0, 0, 14, 0, false, 10e6}, {"6 Msps", 0, 0, 0, 99, 0, false, 6e6},
        {"4 Msps", 0, 0, 0, 99, 0, false, 4e6}, {"all: +25 kHz, -1 %, 100 ns, 15 dB, swapped, 8 Msps", 25e3, 5e3, -10000, 15, 100, true, 8e6},
    };
    for (const Case& k : cs) {
        EbicabGenConfig c = base();
        c.cfoHz = k.cfo; c.driftHzPerSec = k.drift; c.clockPpm = k.ppm; c.ebn0Db = k.ebn0; c.jitterNs = k.jitter; c.invert = k.inv; c.seed = 5;
        const EbicabTelemetry t = run(c, k.rate, 0.5);
        printf("  %-52s %llu telegrams (%llu inverted), %llu bursts\n", k.name, (unsigned long long)t.blocksOk, (unsigned long long)t.invertedTelegrams, (unsigned long long)t.bursts);
        CHECK(t.blocksOk >= 5, "%s: only %llu telegrams", k.name, (unsigned long long)t.blocksOk);
        CHECK(!k.inv || t.invertedTelegrams == t.blocksOk, "%s: inverted %llu of %llu", k.name, (unsigned long long)t.invertedTelegrams, (unsigned long long)t.blocksOk);
        CHECK(t.balises.size() >= 2, "%s: balises %zu", k.name, t.balises.size());
    }
    {   // random bits: bursts listed, no telegram
        EbicabGenConfig c; c.randomBits = true; c.burstSec = 0.0004;   // about 226 bits
        const EbicabTelemetry t = run(c, kRate, 0.4);
        printf("  random bursts: %llu telegrams, %llu bursts listed, first: %d bits %s\n", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.burstLog.empty() ? 0 : t.burstLog[0].bits,
               t.burstLog.empty() ? "" : t.burstLog[0].hint.c_str());
        CHECK(t.blocksOk == 0 && t.balises.empty(), "telegrams from random data");
        CHECK(t.blocksBad >= 8 && !t.burstLog.empty() && t.burstLog[0].bits > 215 && t.burstLog[0].bits < 240, "bursts listed %llu", (unsigned long long)t.blocksBad);
        // a burst of about 255 bits is hinted as EBICab 900 length
        EbicabGenConfig c9; c9.randomBits = true; c9.burstSec = 255 / 564480.0;
        const EbicabTelemetry t9 = run(c9, kRate, 0.4);
        bool hinted = false; for (const auto& b : t9.burstLog) if (b.hint.find("EBICab 900") != std::string::npos) hinted = true;
        CHECK(hinted, "255 bit burst hint");
    }
    {   // noise only
        EbicabGenConfig c; c.randomBits = true; c.burstSec = 1e-9; c.gapSec = 10; c.ebn0Db = 10;
        const EbicabTelemetry t = run(c, kRate, 0.4);
        CHECK(t.blocksOk == 0 && t.blocksBad == 0, "noise: %llu telegrams %llu bursts", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    }
    return testSummary("ebicab rx");
}
