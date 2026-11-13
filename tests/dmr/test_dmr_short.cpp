// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Short DMR transmissions (0.4 to 1 s, a handset in direct mode) through the real receiver, from a cold start and under real-world faults:
// carrier offset, clock offset, noise, a strong neighbour. Measures the share of calls that decode (right IDs, most voice frames) and the delay from
// the start of the call to the first voice burst handed on. 3.2 quality pass; run with an argument to print only (no thresholds).
#include "dect2/dmr/dmr_eval.h"
#include "dect2/dmr/dmr_rx.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "../common/test_long.h"
using namespace dect2;
static int fails = 0;

struct Case { const char* name; double snr, cfo, ppm; bool neighbour; bool mobile; double minShare; double nbDb = 9.5; };

struct Agg { int calls = 0, decoded = 0, anyVoice = 0, falseCalls = 0; double ratioSum = 0, firstSum = 0; int firstN = 0; double firstMax = 0; };

static void runOne(const Case& c, uint32_t seed, Agg& a) {
    DmrGenConfig g;
    g.rate = 1.2e6; g.direct = true; g.mobile = c.mobile; g.snrDb = c.snr; g.cfoHz = c.cfo; g.sroPpm = c.ppm; g.seed = seed; g.traffic = 2;
    if (c.neighbour) g.rms = std::min(0.22, 0.4 / std::pow(10.0, c.nbDb / 20));   // keep the sum inside the 8 bit range
    g.superMin = 1; g.superSpan = 2; g.textMessages = false; g.talkerAlias = false;
    const double secs = 14;
    DmrSignal sig(g);
    std::unique_ptr<DmrSignal> nb;
    if (c.neighbour) { DmrGenConfig n = g; n.cfoHz = 0; n.direct = false; n.snrDb = 40; n.seed = seed + 99; nb = std::make_unique<DmrSignal>(n); }
    DmrReceiver rx;
    rx.setSilent(true);
    rx.configure(g.rate);
    std::vector<double> firstVoice;               // signal time of each voice burst
    double now = 0;
    rx.setVoiceCallback([&](int, int, const uint8_t*) { firstVoice.push_back(now); });
    const size_t chunk = 4800, total = (size_t)(secs * g.rate);
    std::vector<cf32> buf(chunk), nbuf(chunk);
    uint64_t seq = 0;
    DmrTelemetry tel;
    for (size_t done = 0; done < total; done += chunk) {
        sig.generate(buf.data(), chunk);
        if (nb) {
            nb->generate(nbuf.data(), chunk);
            for (size_t i = 0; i < chunk; i++) {
                const double ph = 2 * M_PI * 12500.0 * (double)(done + i) / g.rate;
                buf[i] += nbuf[i] * (float)std::pow(10.0, c.nbDb / 20) * cf32((float)std::cos(ph), (float)std::sin(ph));
            }
        }
        // the radio rounds to 8 bits
        for (size_t i = 0; i < chunk; i++) {
            auto q = [](float x) { return std::round(std::min(127.f, std::max(-128.f, x * 128.f))) / 128.f; };
            buf[i] = cf32(q(buf[i].real()), q(buf[i].imag()));
        }
        now = (double)(done + chunk) / g.rate;
        rx.feed(buf.data(), chunk);
        DmrTelemetry t;
        if (rx.telemetry(t, seq)) { seq = t.seq; tel = t; }
    }
    if (getenv("SB_DEBUG")) printf("  state %d cfo %.0f blocksOk %llu calls %zu cnr %.1f level %.1f\n", tel.state, tel.cfoHz, (unsigned long long)tel.blocksOk, tel.callLog.size(), tel.cnrDb, tel.levelDbfs);
    if (getenv("SB_DEBUG") && !firstVoice.empty() && !sig.truth().empty()) printf("  first voice %.3f s, first call %.3f..%.3f\n", firstVoice[0], sig.truth()[0].startSec, sig.truth()[0].endSec);
    for (const DmrTruth& tr : sig.truth()) {
        if (tr.kind > 2 || !tr.finished || tr.endSec > secs - 1.0) continue;
        a.calls++;
        // the first voice burst handed on at or after the start of the call
        double first = -1;
        for (double v : firstVoice) if (v >= tr.startSec - 0.01 && v < tr.endSec + 0.1) { first = v; break; }
        if (first >= 0) { a.anyVoice++; a.firstSum += first - tr.startSec; a.firstN++; a.firstMax = std::max(a.firstMax, first - tr.startSec); }
        int n = 0;
        for (double v : firstVoice) if (v >= tr.startSec - 0.01 && v < tr.endSec + 0.1) n++;
        const double ratio = std::min(1.0, n / std::max(1.0, (double)tr.voiceBursts));
        a.ratioSum += ratio;
        // decoded: the log has the call with the right IDs, and most of the voice came through
        bool ok = false;
        for (const DmrCall& cl : tel.callLog)
            if (cl.kind <= 2 && std::fabs(cl.startSec - 0.05 - tr.startSec) < 0.6 && cl.idsKnown && cl.src == tr.src && cl.dst == tr.dst && ratio >= 0.8) ok = true;
        if (ok) a.decoded++;
    }
}

int main(int argc, char** argv) {
    const bool printOnly = argc > 1;
    const Case cases[] = {
        {"clean 25 dB", 25, 0, 0, false, false, 0.95},
        {"mobile (to repeater) 25 dB", 25, 0, 0, false, true, 0.95},
        {"cfo +3 kHz", 25, 3000, 0, false, false, 0.9},
        {"cfo -5 kHz, +50 ppm", 25, -5000, 50, false, false, 0.9},
        {"12 dB SNR", 12, 0, 0, false, false, 0.8},
        {"neighbour +0 dB", 25, 0, 0, true, false, 0.95, 0},
        {"neighbour +10 dB", 25, 0, 0, true, false, 0.95, 10},
        {"neighbour +20 dB", 25, 0, 0, true, false, 0.9, 20},
        {"neighbour +30 dB", 25, 0, 0, true, false, 0.0, 30},
        {"cfo +5 kHz", 25, 5000, 0, false, false, 0.9},
        {"cfo -7 kHz", 25, -7000, 0, false, false, 0.8},
        {"6 dB SNR", 6, 0, 0, false, false, 0.8},
    };
    printf("%-30s %6s %8s %8s %9s %9s %9s\n", "case", "calls", "decoded", "voice%", "frames%", "first ms", "max ms");
    // the quick run: 2 seeds, and the middle points left out (12 dB SNR, the neighbour at +0 / +10 dB, cfo +3 kHz); ONAIR_TEST_LONG=1: every case, 6 seeds
    const bool quick[] = {true, true, false, true, false, false, false, true, true, true, true, true};
    static_assert(sizeof quick / sizeof quick[0] == sizeof cases / sizeof cases[0], "one entry per case");
    const uint32_t seeds = testLongPick(2u, 6u);
    for (size_t ci = 0; ci < sizeof cases / sizeof cases[0]; ci++) {
        const Case& c = cases[ci];
        if (argc > 2 && !strstr(c.name, argv[2])) continue;
        if (!testLong() && !quick[ci]) continue;
        Agg a;
        for (uint32_t seed = 1; seed <= seeds; seed++) runOne(c, seed, a);
        const double share = a.calls ? (double)a.decoded / a.calls : 0;
        printf("%-30s %6d %7.0f%% %7.0f%% %8.0f%% %9.0f %9.0f\n", c.name, a.calls, 100 * share, a.calls ? 100.0 * a.anyVoice / a.calls : 0.0, a.calls ? 100 * a.ratioSum / a.calls : 0.0,
               a.firstN ? 1000 * a.firstSum / a.firstN : -1.0, 1000 * a.firstMax);
        if (!printOnly && share < c.minShare) { printf("FAIL: %s: only %.0f%% of short calls decoded\n", c.name, 100 * share); fails++; }
    }
    printf(fails ? "%d FAILED\n" : "done\n", fails);
    return fails ? 1 : 0;
}
