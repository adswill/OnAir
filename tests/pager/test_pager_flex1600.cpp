// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FLEX at 1600 bit/s, two-level FSK (sync code A 0x870C, the most common FLEX speed) through the real receiver: the generator's 1600/2 frame at several
// signal to noise ratios, carrier offsets, clock errors and in both polarities. Also the sync layout at the bit level (the code, its complement and the
// marker as the generator sends them) and the speed table entry. Every page of the frame must arrive with the right text.
#include "dect2/pager/pager_gen.h"
#include "dect2/pager/pager_proto.h"
#include "dect2/pager/pager_rx.h"
#include "../common/impair.h"
#include <cstdio>
#include <functional>
using namespace dect2;
using V = std::vector<cf32>;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static int run(const V& x, double rate, uint64_t& frames, bool& onlyFlex1600) {
    PagerReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(-pagerTuning().tuneOffsetHz);
    PagerTelemetry t; uint64_t seq = 0;
    auto take = [&] { PagerTelemetry n; if (rx.telemetry(n, seq)) { seq = n.seq; t = n; } };
    for (size_t i = 0; i < x.size(); i += 65536) { rx.feed(x.data() + i, std::min<size_t>(65536, x.size() - i)); take(); }
    V z(65536);
    for (int k = 0; k < 8; k++) { rx.feed(z.data(), z.size()); take(); }
    frames = t.speeds[kFlex1600_2].transmissions;
    onlyFlex1600 = true;
    int got = 0;
    if (t.messages) for (const PagerMessage& m : *t.messages) onlyFlex1600 &= m.speed == kFlex1600_2;
    for (const PagerTestMessage& w : pagerTestMessages(0, kFlex1600_2 + 1)) {
        bool f = false;
        if (t.messages) for (const PagerMessage& m : *t.messages) f |= m.flex && m.speed == kFlex1600_2 && m.address == w.address && m.type == w.type && m.text == w.text;
        got += f;
    }
    return got;
}

int main() {
    {   // the speed table: 1600 bit/s 2FSK is sync code A 0x870C at 1600 baud, two levels
        const pager::FlexMode* m = pager::flexModeForSpeed(kFlex1600_2);
        CHECK(m && m->code == 0x870C && m->baud == 1600 && m->levels == 2, "1600/2 table entry");
        const pager::FlexMode* c = pager::flexModeForCode(0x870C ^ 0x0011, 3);   // two bit errors in the sync code
        CHECK(c && c->speed == kFlex1600_2, "1600/2 sync code with two bit errors");
        CHECK(!pager::flexModeForCode(0x870C ^ 0x0FFF, 3) || pager::flexModeForCode(0x870C ^ 0x0FFF, 3)->speed != kFlex1600_2, "a far code is not 1600/2");
        // the complement of A is the data of the second half of sync 1 (code, then the marker, then the inverted code)
        CHECK((uint16_t)~0x870C == 0x78F3, "complement");
    }
    const double rate = 1.2e6;
    const int want = (int)pagerTestMessages(0, kFlex1600_2 + 1).size();
    CHECK(want >= 4, "generator carries %d pages at 1600/2", want);
    struct Case { const char* name; double snr; std::function<void(V&)> f; };
    const std::vector<Case> cases = {
        {"clean 30 dB", 30, [](V&) {}},
        {"12 dB", 12, [](V&) {}},
        {"9 dB", 9, [](V&) {}},
        {"+9 kHz, 20 dB", 20, [&](V& x) { impair::shift(x, 9000, rate); }},
        {"-11 kHz, 20 dB", 20, [&](V& x) { impair::shift(x, -11000, rate); }},
        {"+22 kHz, 25 dB", 25, [&](V& x) { impair::shift(x, 22000, rate); }},
        {"clock +100 ppm, 20 dB", 20, [](V& x) { x = impair::clock(x, 100); }},
        {"clock -100 ppm, 20 dB", 20, [](V& x) { x = impair::clock(x, -100); }},
        {"15 dB", 15, [](V&) {}},
        {"-8 kHz, 20 dB", 20, [&](V& x) { impair::shift(x, -8000, rate); }},
        {"+60 ppm, 15 dB", 15, [](V& x) { x = impair::clock(x, 60); }},
        {"-8 kHz, +60 ppm, 22 dB, echo", 22, [&](V& x) { impair::shift(x, -8000, rate); x = impair::clock(x, 60); impair::echo(x, 37, -6, 2.0); }},
    };
    for (int inv = 0; inv < 2; inv++) {
        for (const Case& cs : cases) {
            SynthConfig c; c.snrDb = cs.snr; c.modeOpt[1] = inv; c.modeOpt[2] = kFlex1600_2 + 1; c.modeOpt[3] = 5;
            auto syn = makePagerSynth(c, rate);
            V x((size_t)(rate * (pagerCycleSeconds(0, kFlex1600_2 + 1) * 1.1 + 0.5)));
            for (size_t i = 0; i < x.size(); i += 65536) syn->generate(x.data() + i, std::min<size_t>(65536, x.size() - i));
            cs.f(x);
            uint64_t frames = 0; bool only = false;
            const int got = run(x, rate, frames, only);
            printf("%s %-26s %d of %d pages, %llu frames\n", inv ? "inverted" : "normal  ", cs.name, got, want, (unsigned long long)frames);
            CHECK(got == want, "%s: %d of %d pages", cs.name, got, want);
            CHECK(only, "%s: a page of another speed appeared", cs.name);
        }
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
