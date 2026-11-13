// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pagers: the 12.5 / 25 kHz channel filter choice. A POCSAG 1200 cycle at 15 dB, carrier offsets swept, with and without a stronger signal
// on the next 12.5 kHz channel (another pager sending address 7777). Both widths must decode every page of the wanted channel when it is
// alone; with the neighbour the narrow filter must keep every page and the wide one must do worse (the point of the setting).
#include "dect2/pager/pager_gen.h"
#include "dect2/pager/pager_rx.h"
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static constexpr double kRate = 1.2e6;
static constexpr double kNeighbourSnr = 30;   // carrier to noise; the wanted channel is 15 dB
static constexpr uint32_t kForeign = 7777;

struct Result { int found = 0, total = 0; bool foreign = false; };

// extraSnr > 0: the neighbour 12.5 kHz up, carrier to noise extraSnr dB (the wanted one is 15 dB)
static Result run(bool narrow, double cfo, double extraSnr) {
    SynthConfig c; c.snrDb = 15; c.cfoHz = cfo; c.modeOpt[0] = 1; c.modeOpt[2] = 2;   // POCSAG 1200 only
    auto syn = makePagerSynth(c, kRate);
    std::unique_ptr<ModeSynth> nb;
    if (extraSnr > 0) {
        SynthConfig n; n.snrDb = extraSnr; n.cfoHz = 12500; n.modeOpt[3] = 7;
        nb = makePagerSynthPage(n, kRate, 2, kForeign, 0, kPagerAlpha, "NEIGHBOUR CHANNEL");
    }
    PagerReceiver rx;
    rx.configure(kRate);
    rx.setNarrowChannel(narrow);
    rx.setProtocols(1);
    rx.setSignalOffset(-pagerTuning().tuneOffsetHz);
    std::vector<cf32> a(65536), b(65536);
    PagerTelemetry t; uint64_t seq = 0;
    const double secs = pagerCycleSeconds(1, 2) + 1.0;
    for (double done = 0; done < secs * kRate; done += (double)a.size()) {
        syn->generate(a.data(), a.size());
        if (nb) { nb->generate(b.data(), b.size()); for (size_t i = 0; i < a.size(); i++) a[i] += b[i]; }
        rx.feed(a.data(), a.size());
        PagerTelemetry n;
        if (rx.telemetry(n, seq)) { seq = n.seq; t = n; }
    }
    Result r;
    const auto want = pagerTestMessages(1, 2);
    const std::vector<PagerMessage> none;
    const std::vector<PagerMessage>& got = t.messages ? *t.messages : none;
    for (const auto& w : want) {
        r.total++;
        for (const auto& m : got) if (m.address == w.address && m.type == w.type && m.text == w.text) { r.found++; break; }
    }
    for (const auto& m : got) if (m.address == kForeign) r.foreign = true;
    return r;
}

int main() {
    for (double cfo : {-3000.0, 0.0, 3000.0}) {
        for (bool narrow : {false, true}) {
            const Result r = run(narrow, cfo, 0);
            printf("alone, %s, offset %+.0f Hz: %d of %d pages\n", narrow ? "12.5 kHz" : "25 kHz", cfo, r.found, r.total);
            CHECK(r.total > 0 && r.found == r.total, "alone %s offset %.0f: %d of %d pages", narrow ? "12.5" : "25", cfo, r.found, r.total);
        }
    }
    for (double cfo : {-1500.0, 0.0, 1500.0}) {
        const Result w = run(false, cfo, kNeighbourSnr), n = run(true, cfo, kNeighbourSnr);
        printf("neighbour, offset %+.0f Hz: 25 kHz %d of %d (foreign %d), 12.5 kHz %d of %d (foreign %d)\n", cfo, w.found, w.total, (int)w.foreign, n.found, n.total, (int)n.foreign);
        CHECK(n.found == n.total, "12.5 kHz with a neighbour, offset %.0f: %d of %d pages", cfo, n.found, n.total);
        CHECK(n.found > w.found, "the narrow filter is not better: %d vs %d pages", n.found, w.found);
    }
    return testSummary("pager_width");
}
