// Whole-span TV: occupied-signal detection (spantv/span_analyzer.h) and tracking (spantv/signal_tracker.h) on synthetic satellite IF captures:
// 2-3 transponders of different symbol rates and roll-offs in 2.4, 8 and 20 Msps, with an IF tilt, 8-bit samples, a -10 dB neighbour, a tone
// that must not count as a signal, and an empty span. Centre, -3 dB width (the symbol rate) within 5 %, the roll-off snapped to the right value
// where the carrier is strong enough; the tracker keeps the ids stable and forgets a carrier that went away.
#include "dect2/engine/spantv/span_analyzer.h"
#include "dect2/engine/spantv/signal_tracker.h"
#include "spantv_testkit.h"
#include <cstdio>
using namespace dect2;
using namespace spantvtest;
#include "../common/test_util.h"

static std::vector<SignalCand> analyse(SatCapture& cap, double rate, double secs, SpanAnalyzer& a, double minBw = 50e3) {
    a.configure(rate, (int)std::max(4096.0, rate / (minBw / 40)), 4);
    std::vector<cf32> blk;
    for (size_t done = 0; done < (size_t)(rate * secs); done += 1 << 16) { cap.pull(1 << 16, blk); a.feed(blk.data(), blk.size()); }
    SpanAnalyzer::Params p; p.minBwHz = minBw;
    return a.detect(p);
}

static void expect(const char* title, const std::vector<SignalCand>& got, const std::vector<SatSpec>& want, bool checkRollOff) {
    printf("%s\n", title);
    for (const auto& c : got)
        printf("  found %+8.1f kHz  bw3 %7.1f kHz  bw %7.1f kHz  shoulder %6.1f kHz  roll-off %.2f  snr %4.1f dB%s\n", c.centerHz / 1e3, c.bw3Hz / 1e3, c.bwHz / 1e3, c.edgeHz / 1e3,
               c.rollOff, c.snrDb, c.clipped ? " (clipped)" : "");
    CHECK(got.size() == want.size(), "%s: %zu signals, want %zu", title, got.size(), want.size());
    for (const SatSpec& w : want) {
        const SignalCand* best = nullptr;
        for (const auto& c : got) if (!best || std::fabs(c.centerHz - w.offsetHz) < std::fabs(best->centerHz - w.offsetHz)) best = &c;
        if (!best) continue;
        CHECK(std::fabs(best->centerHz - w.offsetHz) < 0.03 * w.rs, "%s: centre %+.1f kHz, want %+.1f", title, best->centerHz / 1e3, w.offsetHz / 1e3);
        CHECK(std::fabs(best->rsEstimate / w.rs - 1) < 0.05, "%s: Rs %.1f kBd, want %.1f", title, best->rsEstimate / 1e3, w.rs / 1e3);
        if (checkRollOff) CHECK(std::fabs(best->rollOff - w.rollOff) < 0.01, "%s: roll-off %.2f, want %.2f", title, best->rollOff, w.rollOff);
    }
}

int main() {
    {   // an RTL-SDR: three SCPC carriers in 2.4 Msps, 8-bit
        std::vector<SatSpec> s = {{2, dvbs::kQpsk, 5, 500e3, 0.35, -720e3, 16, 1, "A", false, false, 1},
                                  {1, dvbs::kQpsk, 0, 333e3, 0.35, 0, 16, 2, "B", false, false, 2},
                                  {2, dvbs::k8psk, 4, 400e3, 0.20, 700e3, 18, 3, "C", false, false, 3}};
        SatCapture cap(s, 2.4e6, 0, true);
        SpanAnalyzer a;
        expect("2.4 Msps, three carriers, 8 bit", analyse(cap, 2.4e6, 1.0, a), s, true);
    }
    {   // 8 Msps, roll-offs 0.35 / 0.25 / 0.20, 5 dB of IF tilt
        std::vector<SatSpec> s = {{2, dvbs::kQpsk, 6, 2e6, 0.25, -2.2e6, 15, 1, "A", false, false, 1},
                                  {1, dvbs::kQpsk, 2, 1e6, 0.35, 0, 15, 2, "B", false, false, 2},
                                  {2, dvbs::k8psk, 5, 1.5e6, 0.20, 2.3e6, 18, 3, "C", false, false, 3}};
        SatCapture cap(s, 8e6, 5);
        SpanAnalyzer a;
        expect("8 Msps, three carriers, 5 dB tilt", analyse(cap, 8e6, 0.6, a), s, true);
        const auto fl = a.floorCurveDb();
        const int N = a.bins();
        printf("  floor: %.1f dB at the bottom, %.1f dB at the top of the usable span\n", fl[(size_t)(N * 0.06)], fl[(size_t)(N * 0.94)]);
        CHECK(fl[(size_t)(N * 0.06)] - fl[(size_t)(N * 0.94)] > 3, "the floor follows the tilt");
    }
    {   // 20 Msps: a big and two small transponders, one of them 10 dB weaker
        std::vector<SatSpec> s = {{2, dvbs::kQpsk, 5, 5e6, 0.35, -5.5e6, 16, 1, "A", false, false, 1},
                                  {1, dvbs::kQpsk, 2, 2e6, 0.35, 0, 9, 2, "B", false, false, 2},
                                  {2, dvbs::k8psk, 5, 3e6, 0.25, 5e6, 18, 3, "C", false, false, 3}};
        SatCapture cap(s, 20e6, 2);
        SpanAnalyzer a;
        const auto got = analyse(cap, 20e6, 0.3, a);
        expect("20 Msps, three carriers (one 9 dB down)", got, s, false);
    }
    {   // noise only, and noise with a tone: nothing
        std::vector<SatSpec> none;
        SatCapture cap(none, 2.4e6);
        SpanAnalyzer a;
        a.configure(2.4e6, 4096, 4);
        std::vector<cf32> blk;
        for (int i = 0; i < 40; i++) {
            cap.pull(1 << 16, blk);
            for (size_t k = 0; k < blk.size(); k++) { const double ph = 2 * M_PI * 300e3 * (double)(i * 65536 + (int)k) / 2.4e6; blk[k] += 0.05f * cf32((float)std::cos(ph), (float)std::sin(ph)); }
            a.feed(blk.data(), blk.size());
        }
        SpanAnalyzer::Params p; p.minBwHz = 50e3;
        const auto got = a.detect(p);
        CHECK(got.empty(), "noise and a tone: %zu signals", got.size());
    }
    {   // the tracker: stable ids over evaluations, a vanished carrier pruned, a new one gets a new id
        SignalTracker t;
        t.setBinHz(600);
        auto c = [](double f, double bw) { SignalCand s; s.centerHz = f; s.bw3Hz = bw; s.bwHz = bw * 1.35; return s; };
        t.update({c(-700e3, 500e3), c(0, 333e3)});
        const auto& a = t.update({c(-701e3, 498e3), c(1e3, 335e3)});
        CHECK(a.size() == 2 && a[0].present && a[1].present, "two present after two evaluations");
        const int idA = a[0].id, idB = a[1].id;
        for (int i = 0; i < 8; i++) t.update({c(-700e3, 500e3)});   // B fades away
        t.prune(8, nullptr);
        CHECK(t.tracks().size() == 1 && t.tracks()[0].id == idA, "B pruned, A kept its id");
        t.update({c(-700e3, 500e3), c(700e3, 400e3)});
        const TrackedSignal* n = nullptr;
        for (const auto& x : t.tracks()) if (x.id != idA) n = &x;
        CHECK(n && n->id != idB && !n->present, "a new carrier gets a new id, present only after a second sighting");
        // one that is held by a receiver is not pruned
        for (int i = 0; i < 9; i++) t.update({});
        t.prune(8, [idA](int id) { return id == idA; });
        CHECK(t.tracks().size() == 1 && t.tracks()[0].id == idA, "a signal with a receiver is kept through a fade");
    }
    return testSummary("spantv_analyzer");
}
