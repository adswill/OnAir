// Pagers under real-radio faults (REAL_WORLD_CHECKLIST.md): one generator cycle (every POCSAG and FLEX speed) through tests/impair.h, then the
// real receiver, in both polarities. A radio 50 ppm off is 8 kHz at 160 MHz and 23 kHz at 460 MHz. Every page of the cycle must arrive.
#include "dect2/pager_gen.h"
#include "dect2/pager_rx.h"
#include "impair.h"
#include <cstdio>
#include <functional>
using namespace dect2;
using V = std::vector<cf32>;
static int fails = 0;

static int run(const V& x, double rate, double& cfo) {
    PagerReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(-pagerTuning().tuneOffsetHz);
    PagerTelemetry t; uint64_t seq = 0;
    auto take = [&] { PagerTelemetry n; if (rx.telemetry(n, seq)) { seq = n.seq; t = n; } };
    for (size_t i = 0; i < x.size(); i += 65536) { rx.feed(x.data() + i, std::min<size_t>(65536, x.size() - i)); take(); }
    V z(65536);
    for (int k = 0; k < 8; k++) { rx.feed(z.data(), z.size()); take(); }
    cfo = t.cfoHz;
    int got = 0;
    for (const PagerTestMessage& w : pagerTestMessages()) {
        bool f = false;
        if (t.messages) for (const PagerMessage& m : *t.messages) f |= m.address == w.address && m.speed == w.speed && m.text == w.text;
        got += f;
    }
    return got;
}

int main() {
    const double rate = 1.2e6;
    const int want = (int)pagerTestMessages().size();
    const std::vector<std::pair<const char*, std::function<void(V&)>>> cases = {
        {"carrier +10 kHz", [&](V& x) { impair::shift(x, 10000, rate); }},
        {"carrier -12 kHz", [&](V& x) { impair::shift(x, -12000, rate); }},
        {"carrier +22 kHz", [&](V& x) { impair::shift(x, 22000, rate); }},
        {"carrier -25 kHz", [&](V& x) { impair::shift(x, -25000, rate); }},
        {"clock +100 ppm", [](V& x) { x = impair::clock(x, 100); }},
        {"clock -100 ppm", [](V& x) { x = impair::clock(x, -100); }},
        {"combined: +22 kHz, +80 ppm, echo, 8-bit clipped", [&](V& x) { impair::shift(x, 22000, rate); x = impair::clock(x, 80); impair::echo(x, 37, -6, 2.0); impair::clip8(x, 6); }},
    };
    for (int inv = 0; inv < 2; inv++) {
        SynthConfig c; c.snrDb = 25; c.modeOpt[1] = inv;
        auto syn = makePagerSynth(c, rate);
        V base((size_t)(rate * (pagerCycleSeconds() + 1)));
        for (size_t i = 0; i < base.size(); i += 65536) syn->generate(base.data() + i, std::min<size_t>(65536, base.size() - i));
        for (const auto& [name, f] : cases) {
            V x = base; f(x);
            double cfo = 0;
            const int got = run(x, rate, cfo);
            printf("%s %-48s %d of %d pages, carrier %.0f Hz\n", inv ? "inverted" : "normal  ", name, got, want, cfo);
            if (got != want) { printf("FAIL: %s\n", name); fails++; }
        }
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
