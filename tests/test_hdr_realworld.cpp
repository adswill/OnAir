// HD Radio against the faults of REAL_WORLD_CHECKLIST.md, applied with tests/impair.h (written independently of the receiver) to the generator's
// clean signal: 50 ppm tuning errors (+-5.4 kHz in band II; AM: a few hundred Hz), +-100 ppm sample clocks, swapped I/Q, NaN samples, a USB
// drop, and one combined case per band (worst offset, +80 ppm, an echo and 8-bit clipping).
#include "dect2/hdr_gen.h"
#include "dect2/hdr_rx.h"
#include "impair.h"
#include "jobs.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>
using namespace dect2;
using testjobs::jprintf;
static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL: " __VA_ARGS__); jprintf("\n"); fails++; } } while (0)

static const double kRate = 1e6;

static std::vector<cf32> clean(int mode, double secs) {
    SynthConfig sc;
    sc.mode = 23; sc.modeOpt[0] = mode; sc.snrDb = 30;
    auto syn = makeHdrSynth(sc, kRate);
    std::vector<cf32> x((size_t)(secs * kRate));
    syn->generate(x.data(), x.size());
    return x;
}

static void check(const char* what, int mode, const std::vector<cf32>& x, double cfo = std::numeric_limits<double>::quiet_NaN(), unsigned maxBad = 3) {
    HdrReceiver rx;
    rx.configure(kRate);
    for (size_t i = 0; i < x.size(); i += 20000) rx.feed(x.data() + i, std::min<size_t>(20000, x.size() - i));
    HdrTelemetry t;
    rx.telemetry(t, 0);
    jprintf("%-44s state %d band %d, CFO %+7.1f Hz, P1 %llu ok %llu bad, call sign '%s'\n", what, t.state, t.band, t.cfoHz, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
            t.callSign.c_str());
    CHECK(t.state == 3 && t.band == (mode == 1 ? 2 : 1) && t.blocksOk >= (mode == 1 ? 10u : 5u) && (mode == 1 ? t.blocksBad * 4 <= t.blocksOk : t.blocksBad <= maxBad) &&   // AM: a few frames fail while the receiver settles
          !t.callSign.empty(), "%s", what);
    if (!std::isnan(cfo)) CHECK(std::fabs(t.cfoHz - cfo) < 20, "%s: CFO %.1f, sent %.1f", what, t.cfoHz, cfo);
}

int main() {
    testjobs::Jobs jobs;
    const double fmSecs = 12, amSecs = 16;
    for (double hz : {5400.0, -5400.0}) jobs.add([=] {
        auto x = clean(0, fmSecs); impair::shift(x, hz, kRate);
        char b[64]; snprintf(b, sizeof b, "FM, tuning error %+.0f Hz", hz);
        check(b, 0, x, hz);
    });
    for (double hz : {400.0, -400.0}) jobs.add([=] {
        auto x = clean(1, amSecs); impair::shift(x, hz, kRate);
        char b[64]; snprintf(b, sizeof b, "AM, tuning error %+.0f Hz", hz);
        check(b, 1, x, hz);
    });
    for (int m : {0, 1}) for (double ppm : {-100.0, 100.0}) jobs.add([=] {
        auto x = impair::clock(clean(m, m ? amSecs : fmSecs), ppm); impair::shift(x, 150, kRate);
        char b[64]; snprintf(b, sizeof b, "%s, sample clock %+.0f ppm", m ? "AM" : "FM", ppm);
        check(b, m, x);
    });
    for (int m : {0, 1}) jobs.add([=] {
        auto x = clean(m, 30); impair::shift(x, 230, kRate); impair::swapIq(x);   // the other side is tried after 12 s without a P1 frame
        check(m ? "AM, swapped I/Q" : "FM, swapped I/Q", m, x, std::numeric_limits<double>::quiet_NaN(), 12);   // frames before the switch fail
    });
    jobs.add([=] {
        auto x = clean(0, fmSecs);
        for (size_t i = 0; i < 4000; i++) x[(size_t)(3 * kRate) + i] = cf32(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity());
        check("FM, NaN and infinite samples", 0, x);
    });
    jobs.add([=] { auto x = clean(0, 20); impair::skip(x, 77777); impair::drop(x, (size_t)(4 * kRate), 31415); check("FM, start mid-frame, samples lost", 0, x); });
    jobs.add([=] {
        auto x = impair::clock(clean(0, fmSecs), 80); impair::shift(x, 5400, kRate); impair::echo(x, 3, -6, 2.0); impair::clip8(x, 2.0);
        check("FM combined: +5.4 kHz, +80 ppm, echo, 8 bit", 0, x, 5400);
    });
    jobs.add([=] {
        auto x = impair::clock(clean(1, amSecs), 80); impair::shift(x, 400, kRate); impair::echo(x, 3, -10, 2.0); impair::clip8(x, 2.0);
        check("AM combined: +400 Hz, +80 ppm, echo, 8 bit", 1, x, 400);
    });
    jobs.run();
    if (fails) { printf("%d check(s) failed\n", fails.load()); return 1; }
    printf("OK\n");
    return 0;
}
