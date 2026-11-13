// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CDR must recover by itself from what a real radio link does (issue #138: the sound stopped for good after a "fluctuation" and only a
// restart brought it back). Each case damages the generator's clean hybrid signal (digital sidebands beside an analogue FM programme) and
// checks that the multiplex and the audio units keep arriving afterwards:
//   samples lost in the USB link (the beacon timing steps by hundreds of samples, more than half a beacon too),
//   level steps and deep fades, a carrier step, bursts of noise, junk before the signal (a lock on the FM's lines),
//   the Re-sync request, and a retune (reset) with the listened service picked before.
// The numbers are logical frames (0.64 s each) decoded in a window after the last event.
#include "dect2/cdr/cdr_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/cdr/cdr_rx.h"
#include "../common/impair.h"
#include "../common/jobs.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2;
using testjobs::jprintf;
static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL: " __VA_ARGS__); jprintf("\n"); fails++; } } while (0)

static const double kRate = 2e6;

static std::vector<cf32> clean(double secs) {
    static const int hyb[8] = {3, 3, 2, 0, 1, 3, 0, 0};      // 3/9 64QAM + FM, as in test_cdr_realworld
    SynthConfig sc;
    for (int i = 0; i < 8; i++) sc.modeOpt[i] = hyb[i];
    sc.snrDb = 30;
    auto syn = makeCdrSynth(sc, kRate);
    std::vector<cf32> x((size_t)(secs * kRate));
    syn->generate(x.data(), x.size());
    return x;
}

struct Mark { double t; uint64_t mux, units, unlocks; int state; };

// feeds the samples, listens to service 0x1001, and notes the counters every `step` seconds; `at` may act on the receiver (resync, reset)
template <class F> static std::vector<Mark> play(CdrReceiver& rx, const std::vector<cf32>& x, F at, double step = 0.5) {
    std::vector<Mark> m;
    rx.setMuted(true);
    rx.setVolume(0);
    rx.setListen(0x1001);
    size_t next = 0;
    for (size_t i = 0; i < x.size(); i += 25000) {
        at(rx, (double)i / kRate);
        rx.feed(x.data() + i, std::min<size_t>(25000, x.size() - i));
        if (i >= next) {
            CdrTelemetry t;
            rx.telemetry(t, 0);
            m.push_back({(double)i / kRate, t.muxOk, t.draUnitsIn, t.unlocks, t.state});
            next = i + (size_t)(step * kRate);
        }
    }
    CdrTelemetry t;
    rx.telemetry(t, 0);
    m.push_back({(double)x.size() / kRate, t.muxOk, t.draUnitsIn, t.unlocks, t.state});
    return m;
}
static const Mark& markAt(const std::vector<Mark>& m, double t) {
    for (const auto& k : m) if (k.t >= t) return k;
    return m.back();
}
// multiplex frames and audio units that arrived between two moments
static void window(const char* what, const std::vector<Mark>& m, double a, double b, uint64_t needMux, uint64_t needUnits) {
    const Mark &p = markAt(m, a), &q = markAt(m, b);
    jprintf("%-46s %5.1f..%5.1f s: %3llu frames, %4llu audio units (need %llu / %llu), unlocks %llu\n", what, p.t, q.t, (unsigned long long)(q.mux - p.mux),
            (unsigned long long)(q.units - p.units), (unsigned long long)needMux, (unsigned long long)needUnits, (unsigned long long)q.unlocks);
    CHECK(q.mux - p.mux >= needMux && q.units - p.units >= needUnits, "%s: only %llu frames / %llu audio units in %.1f..%.1f s", what, (unsigned long long)(q.mux - p.mux),
          (unsigned long long)(q.units - p.units), a, b);
}
static CdrReceiver* fresh(std::unique_ptr<CdrReceiver>& p) { p = std::make_unique<CdrReceiver>(); p->configure(kRate); return p.get(); }
static void none(CdrReceiver&, double) {}

int main() {
    testjobs::Jobs jobs;
    // 735 samples at 2 Msps are 300 at the receiver's 816 ksps: what the logs of the reporters showed every two seconds
    jobs.add([] {
        auto x = clean(20);
        for (double t : {10.0, 8.0, 6.0, 4.0}) impair::drop(x, (size_t)(t * kRate), 735);
        std::unique_ptr<CdrReceiver> p; auto m = play(*fresh(p), x, none);
        window("4 x 735 samples lost (steps of 300)", m, 13.0, 19.5, 6, 90);
        window("... the whole run", m, 3.0, 19.5, 14, 200);
    });
    jobs.add([] {   // more than half a beacon (a step the beacon repeats around): the pilots pick the right alignment, or the search starts again
        auto x = clean(22);
        impair::drop(x, (size_t)(4.0 * kRate), 1500);
        impair::drop(x, (size_t)(7.0 * kRate), 6000);
        impair::drop(x, (size_t)(10.0 * kRate), 40000);
        std::unique_ptr<CdrReceiver> p; auto m = play(*fresh(p), x, none);
        window("drops of 1500, 6000 and 40000 samples", m, 16.0, 21.5, 5, 70);
    });
    jobs.add([] {   // level steps of the radio's gain and a deep fade, 20 .. 40 dB, 50 .. 500 ms
        auto x = clean(20);
        auto gain = [&](double t0, double t1, double db) { const float g = (float)std::pow(10.0, db / 20); for (size_t i = (size_t)(t0 * kRate); i < (size_t)(t1 * kRate) && i < x.size(); i++) x[i] *= g; };
        gain(3.0, 3.05, -40); gain(5.0, 5.5, -30); gain(7.0, 9.0, -20); gain(10.0, 10.2, +20); gain(12.0, 12.3, -35);
        std::unique_ptr<CdrReceiver> p; auto m = play(*fresh(p), x, none);
        window("level steps and fades of 20-40 dB", m, 15.0, 19.5, 5, 70);
    });
    jobs.add([] {   // the carrier steps (the tuner's PLL re-locks): 4 kHz at 5 s, back at 9 s
        auto x = clean(20);
        std::vector<cf32> mid(x.begin() + (size_t)(5 * kRate), x.begin() + (size_t)(9 * kRate));
        impair::shift(mid, 4000, kRate);
        std::copy(mid.begin(), mid.end(), x.begin() + (size_t)(5 * kRate));
        std::unique_ptr<CdrReceiver> p; auto m = play(*fresh(p), x, none);
        window("carrier steps by 4 kHz and back", m, 14.0, 19.5, 5, 70);
    });
    jobs.add([] {   // bursts of strong noise (ignition, a neighbouring transmitter)
        auto x = clean(20);
        std::mt19937 g(5); dect2::normal_distribution<float> n(0, 6.f);
        for (double t : {4.0, 7.0, 10.0}) for (size_t i = (size_t)(t * kRate); i < (size_t)((t + 0.15) * kRate); i++) x[i] += cf32{n(g), n(g)};
        std::unique_ptr<CdrReceiver> p; auto m = play(*fresh(p), x, none);
        window("bursts of noise 50 dB over the signal", m, 14.0, 19.5, 5, 70);
    });
    jobs.add([] {   // 8 s of an FM-like programme alone (carrier, 19 kHz pilot, 38 kHz stereo) and then the CDR signal appears
        const double junk = 8;
        std::vector<cf32> x((size_t)(junk * kRate));
        std::mt19937 g(9); dect2::normal_distribution<float> n(0, 1.f);
        double ph = 0, lp = 0, lp2 = 0;
        for (size_t i = 0; i < x.size(); i++) {
            const double t = (double)i / kRate;
            lp = 0.995 * lp + 0.005 * n(g); lp2 = 0.995 * lp2 + 0.005 * n(g);
            const double mono = 12 * lp, diff = 12 * lp2;
            const double m = 0.45 * mono + 0.45 * diff * std::sin(2 * M_PI * 38e3 * t) + 0.09 * std::sin(2 * M_PI * 19e3 * t);
            ph += 2 * M_PI * 75e3 * m / kRate;
            x[i] = cf32((float)std::cos(ph), (float)std::sin(ph)) * 0.3f;
        }
        auto c = clean(14);
        x.insert(x.end(), c.begin(), c.end());
        std::unique_ptr<CdrReceiver> p; auto m = play(*fresh(p), x, none);
        window("8 s of FM alone, then the CDR signal", m, junk + 7.0, junk + 13.5, 6, 80);
    });
    jobs.add([] {   // the Re-sync button: the lock is dropped at once and found again; the listened service keeps playing
        auto x = clean(18);
        std::unique_ptr<CdrReceiver> p; bool done = false;
        auto m = play(*fresh(p), x, [&](CdrReceiver& r, double t) { if (!done && t >= 8.0) { done = true; r.resync(); } });
        window("before the Re-sync", m, 4.0, 8.0, 4, 50);
        window("after the Re-sync", m, 12.0, 17.5, 5, 70);
        CdrTelemetry t; p->telemetry(t, 0);
        CHECK(t.resyncs == 1, "one Re-sync counted (%llu)", (unsigned long long)t.resyncs);
    });
    jobs.add([] {   // a retune (reset): everything is forgotten, the service picked before is played again once it is listed
        auto x = clean(18);
        std::unique_ptr<CdrReceiver> p; bool done = false;
        auto m = play(*fresh(p), x, [&](CdrReceiver& r, double t) { if (!done && t >= 8.0) { done = true; r.reset(); } });
        window("after the retune", m, 12.0, 17.5, 5, 70);
    });
    jobs.add([] {   // a 6 s dropout (the link stops): the lock is lost, the beacon is searched again after it, and the sound comes back and keeps going
        auto x = clean(22);
        for (size_t i = (size_t)(4.0 * kRate); i < (size_t)(10.0 * kRate); i++) x[i] = cf32(0, 0);
        std::unique_ptr<CdrReceiver> p; auto m = play(*fresh(p), x, none);
        window("6 s dropout, before it", m, 2.0, 4.0, 4, 50);
        window("6 s dropout, after it (relocked)", m, 14.0, 19.5, 5, 70);
        CdrTelemetry t; p->telemetry(t, 0);
        CHECK(t.unlocks >= 1, "the dropout must drop the lock (%llu unlocks)", (unsigned long long)t.unlocks);
        CHECK(t.locks >= 2, "the receiver must lock again after the dropout (%llu locks)", (unsigned long long)t.locks);
    });
    jobs.add([] {   // an overloaded input (gain too high, samples at full scale): the receiver says so, and still decodes what it can
        auto x = clean(14);
        double pw = 0;
        for (auto& v : x) pw += std::norm(v);
        const float g = 0.9f / (float)std::sqrt(pw / (double)x.size());      // rms 0.9 of full scale on each axis ~0.64: well into the limiter
        for (auto& v : x) v = cf32(std::max(-1.f, std::min(1.f, v.real() * g)), std::max(-1.f, std::min(1.f, v.imag() * g)));
        CdrReceiver rx; rx.configure(kRate);
        std::atomic<int> said{0};
        rx.setLogCallback([&](const std::string& s) { if (s.find("overloaded") != std::string::npos) said++; });
        play(rx, x, none);
        CdrTelemetry t; rx.telemetry(t, 0);
        jprintf("overloaded input: %.1f %% clipped, message said %d time(s)\n", t.clipPct, said.load());
        CHECK(t.clipPct > 2.f, "clipping measured (%.1f %%)", t.clipPct);
        CHECK(said >= 1, "an overload message must be logged");
        auto y = clean(6);
        CdrReceiver r2; r2.configure(kRate);
        std::atomic<int> said2{0};
        r2.setLogCallback([&](const std::string& s) { if (s.find("overloaded") != std::string::npos) said2++; });
        play(r2, y, none);
        CHECK(said2 == 0, "no overload message on a clean signal");
    });
    jobs.run();
    if (fails) { printf("%d check(s) failed\n", fails.load()); return 1; }
    printf("OK\n");
    return 0;
}
