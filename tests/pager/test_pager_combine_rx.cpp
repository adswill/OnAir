// Pagers: multi-copy combining on the full receiver (#20). A page repeats on air (POCSAG 1200) while 6 ms bursts of strong noise at random times wreck
// a code word or two of each copy; some are repaired into wrong words, some cut the page short. The receiver with combining must give clearly more
// complete, correct copies and fewer wrong ones than the one without. Two seeds and two carrier offsets, 30 s of signal each.
#include "dect2/pager/pager_gen.h"
#include "dect2/pager/pager_rx.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct Tally { int complete = 0, damaged = 0, wrong = 0, combined = 0, total = 0; };

static Tally run(bool combine, int speed, double snr, double cfo, int seed, const std::string& text, double seconds, double burstMs, float burstAmp) {
    const double rate = 1.2e6;
    PagerReceiver rx;
    rx.configure(rate);
    rx.setCombineCopies(combine);
    rx.setSignalOffset(-pagerTuning().tuneOffsetHz);
    SynthConfig cfg;
    cfg.snrDb = snr; cfg.cfoHz = cfo;
    cfg.modeOpt[3] = seed;
    auto syn = makePagerSynthPage(cfg, rate, speed, 1234567, 3, kPagerAlpha, text);
    std::vector<cf32> buf(65536);
    uint32_t lcgS = 777u + (uint32_t)seed * 7919u;
    auto lcg = [&] { lcgS = lcgS * 1664525u + 1013904223u; return lcgS >> 8; };
    const uint64_t burstLen = (uint64_t)(rate * burstMs / 1000.0);
    uint64_t nIdx = 0, nextBurst = (uint64_t)(rate * 0.3);
    PagerTelemetry t;
    uint64_t seq = 0;
    for (double done = 0; done < seconds * rate; done += (double)buf.size()) {
        syn->generate(buf.data(), buf.size());
        // impulsive interference (a different place in every copy): a few milliseconds of strong noise now and then wreck a code word or two
        for (size_t i = 0; i < buf.size(); i++, nIdx++) {
            if (nIdx >= nextBurst + burstLen) { nextBurst = nIdx + (uint64_t)(rate * (0.25 + 0.5 * (lcg() % 1000) / 1000.0)); }
            if (nIdx >= nextBurst) buf[i] = cf32(((lcg() % 2000) / 1000.f - 1.f) * burstAmp, ((lcg() % 2000) / 1000.f - 1.f) * burstAmp);
        }
        rx.feed(buf.data(), buf.size());
        PagerTelemetry n;
        if (rx.telemetry(n, seq)) { seq = n.seq; t = n; }
    }
    Tally r;
    if (t.messages)
        for (const PagerMessage& m : *t.messages) {
            if (m.address != 1234567 || m.flex) continue;
            r.total++;
            if (m.text == text && !m.damaged) r.complete++;
            else if (m.damaged) r.damaged++;
            else r.wrong++;
            if (m.combined) r.combined++;
        }
    return r;
}

int main() {
    const std::string text = "FIRE ALARM Station 7 Main Street request crews now";
    Tally off, on;
    for (double bms : {6.0})
    for (int speed : {kPocsag1200})
        for (double snr : {15.0})
            for (double cfo : {0.0, 600.0})
                for (int seed = 1; seed <= 2; seed++) {
                    const Tally a = run(false, speed, snr, cfo, seed, text, 30, bms, 4.f);
                    const Tally b = run(true, speed, snr, cfo, seed, text, 30, bms, 4.f);
                    off.complete += a.complete; off.damaged += a.damaged; off.wrong += a.wrong; off.total += a.total;
                    on.complete += b.complete; on.damaged += b.damaged; on.wrong += b.wrong; on.total += b.total; on.combined += b.combined;
                }
    printf("  combining off: %d complete, %d damaged, %d wrong of %d pages\n  combining on:  %d complete, %d damaged, %d wrong of %d pages (%d repaired from copies)\n",
           off.complete, off.damaged, off.wrong, off.total, on.complete, on.damaged, on.wrong, on.total, on.combined);
    CHECK(off.total > 0 && on.total > 0, "pages were received");
    CHECK(on.complete > off.complete && on.complete >= off.complete + off.wrong / 2, "combining gives no fewer complete pages (%d vs %d)", on.complete, off.complete);
    CHECK(on.wrong <= off.wrong, "combining makes no wrong text (%d vs %d)", on.wrong, off.wrong);
    return testSummary("pager_combine_rx");
}
