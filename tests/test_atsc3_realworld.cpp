// ATSC 3.0 against real-world faults (REAL_WORLD_CHECKLIST.md): the clean test signal (atsc3_synth.h), impaired with tests/impair.h (written
// independently of the receiver), through the real Atsc3Rx. Every case must find and decode nearly every frame.
//   test_atsc3_realworld          all cases
//   test_atsc3_realworld N        only case N
#include "dect2/atsc3_rx.h"
#include "dect2/atsc3_synth.h"
#include "impair.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
#include <vector>
using namespace dect2;

static int fails = 0;
static constexpr double kRate = 8e6;

static std::vector<cf32> clean(double seconds, int fecFrame = 0) {
    SynthConfig cfg;
    cfg.snrDb = 200;
    cfg.modeOpt[2] = fecFrame;
    auto synth = makeAtsc3Synth(cfg, kRate);
    std::vector<cf32> x((size_t)(seconds * kRate));
    if (!synth) { x.clear(); return x; }
    for (size_t i = 0; i < x.size(); i += 65536) synth->generate(x.data() + i, std::min<size_t>(65536, x.size() - i));
    return x;
}

static void clipTo(std::vector<cf32>& x, double rms) {
    double p = 0;
    for (auto& v : x) p += std::norm(v);
    impair::clip8(x, rms / std::sqrt(p / (double)x.size()));
}

struct Res { long frames = 0, failed = 0, bb = 0, bbBad = 0; double cfo = 0; long bootstraps = 0, dropped = 0; };

static Res receive(const std::vector<cf32>& x) {
    Atsc3Rx rx;
    rx.setPacketCallback([](const uint8_t*, size_t, double) {});
    rx.setBlocking(true);
    rx.configure(kRate);
    for (size_t i = 0; i < x.size(); i += 65536) rx.feed(x.data() + i, std::min<size_t>(65536, x.size() - i));
    Atsc3Telemetry t;
    long last = -1;
    int same = 0;
    for (int i = 0; i < 100; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        rx.telemetry(t, 0);
        same = t.frames == last ? same + 1 : 0;
        if (same >= 4) break;
        last = t.frames;
    }
    rx.telemetry(t, 0);
    rx.stop();
    return {t.frames, t.framesFailed, t.bbPackets, t.bbBad, t.cfoHz, t.bootstraps, t.droppedBlocks};
}

struct Case { const char* name; double seconds; std::function<void(std::vector<cf32>&)> fault; };

int main(int argc, char** argv) {
    const int only = argc > 1 ? atoi(argv[1]) : -1;
    std::vector<Case> cs = {
        {"clean", 3.0, [](auto&) {}},
        {"UHF tuning error +43 kHz", 3.0, [](auto& x) { impair::shift(x, 43000, kRate); }},
        {"UHF tuning error -43 kHz", 3.0, [](auto& x) { impair::shift(x, -43000, kRate); }},
        {"channel 0.9 MHz off centre", 3.0, [](auto& x) { impair::shift(x, 900000, kRate); }},
        {"sample clock +100 ppm", 3.0, [](auto& x) { x = impair::clock(x, 100); }},
        {"sample clock -100 ppm", 3.0, [](auto& x) { x = impair::clock(x, -100); }},
        {"start mid-frame, USB drop", 3.0, [](auto& x) { impair::skip(x, 300001); impair::drop(x, 9000000, 5555); }},
        {"combined: +43 kHz, +80 ppm, echo, 8-bit", 3.0, [](auto& x) {
             impair::echo(x, 40, -8.0, 1.0); x = impair::clock(x, 80); impair::shift(x, 43000, kRate); clipTo(x, 0.3); }},
    };
    const double perFrame = 0.1025;
    for (size_t i = 0; i < cs.size(); i++) {
        if (only >= 0 && (int)i != only) continue;
        auto& c = cs[i];
        auto x = clean(c.seconds);
        if (x.empty()) { printf("no generator\n"); return 1; }
        c.fault(x);
        const Res r = receive(x);
        const long expect = (long)(c.seconds / perFrame) - 7;   // the first bootstrap, the clock, and the frames still in flight at the end
        const bool ok = r.frames - r.failed >= expect && r.bb > 0 && r.bbBad <= 6;   // the frames before the clock has been measured may lose a few
        printf("%-44s frames %3ld failed %2ld (want %ld good)  baseband %5ld bad %3ld  cfo %+.0f Hz  bootstraps %ld dropped %ld  %s\n", c.name, r.frames, r.failed, expect, r.bb, r.bbBad, r.cfo, r.bootstraps, r.dropped, ok ? "ok" : "FAIL");
        if (!ok) fails++;
    }
    printf(fails ? "ATSC 3.0 real-world tests FAILED\n" : "ATSC 3.0 real-world tests passed\n");
    return fails ? 1 : 0;
}
