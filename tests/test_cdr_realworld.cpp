// CDR against the faults of REAL_WORLD_CHECKLIST.md, applied with tests/impair.h (written independently of the receiver) to the generator's clean
// signal: 50 ppm tuning errors in band II (+-5.4 kHz at 108 MHz), +-100 ppm sample clocks, swapped I/Q, NaN samples, a USB drop and one
// combined case (+5.4 kHz, +80 ppm, an echo and 8-bit clipping).
#include "dect2/cdr_gen.h"
#include "dect2/cdr_rx.h"
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

static const double kRate = 2e6;

static std::vector<cf32> clean(const int* opt, double secs) {
    SynthConfig sc;
    for (int i = 0; i < 8; i++) sc.modeOpt[i] = opt[i];
    sc.snrDb = 30;
    auto syn = makeCdrSynth(sc, kRate);
    std::vector<cf32> x((size_t)(secs * kRate));
    syn->generate(x.data(), x.size());
    return x;
}

static void check(const char* what, const std::vector<cf32>& x, double cfo = std::numeric_limits<double>::quiet_NaN()) {
    CdrReceiver rx;
    rx.configure(kRate);
    for (size_t i = 0; i < x.size(); i += 50000) rx.feed(x.data() + i, std::min<size_t>(50000, x.size() - i));
    CdrTelemetry t;
    rx.telemetry(t, 0);
    jprintf("%-44s state %d, CFO %+7.1f Hz, SDC %llu/%llu, LDPC %llu ok %llu bad, %zu services\n", what, t.state, t.cfoHz, (unsigned long long)t.sdcOk, (unsigned long long)t.sdcBad,
            (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.services.size());
    CHECK(t.state == 3 && t.sdcOk >= 1 && t.blocksOk >= 10 && t.blocksBad * 10 <= t.blocksOk && t.services.size() >= 2, "%s", what);
    if (!std::isnan(cfo)) CHECK(std::fabs(t.cfoHz - cfo) < 20, "%s: CFO %.1f, sent %.1f", what, t.cfoHz, cfo);
}

int main() {
    static const int dig[8] = {0, 0, 0, 0, 0, 0, 0, 0};      // 1/1 100 kHz all digital
    static const int hyb[8] = {3, 3, 2, 0, 1, 3, 0, 0};      // 3/9 64QAM + FM
    testjobs::Jobs jobs;
    for (double hz : {5400.0, -5400.0}) jobs.add([=] {
        auto x = clean(dig, 4); impair::shift(x, hz, kRate);
        char b[64]; snprintf(b, sizeof b, "tuning error %+.0f Hz", hz);
        check(b, x, hz);
    });
    jobs.add([=] { auto x = clean(hyb, 6.5); impair::shift(x, -5400, kRate); check("hybrid with FM, tuning error -5400 Hz", x, -5400); });
    jobs.add([=] { auto x = clean(hyb, 6.5); impair::shift(x, -12000, kRate); check("hybrid with FM, tuning error -12 kHz", x, -12000); });
    for (double ppm : {-100.0, 100.0}) jobs.add([=] {
        auto x = impair::clock(clean(dig, 4), ppm); impair::shift(x, 1000, kRate);
        char b[64]; snprintf(b, sizeof b, "sample clock %+.0f ppm", ppm);
        check(b, x);
    });
    // the signal away from the middle of the sample band (a recording made at an offset)
    jobs.add([=] { auto x = clean(dig, 10); impair::shift(x, 520000, kRate); check("off centre +520 kHz", x, 520000); });
    jobs.add([=] { auto x = clean(hyb, 14); impair::shift(x, -380000, kRate); check("hybrid with FM, off centre -380 kHz", x, -380000); });
    jobs.add([=] { auto x = clean(dig, 4); impair::shift(x, 1700, kRate); impair::swapIq(x); check("swapped I/Q", x); });
    jobs.add([=] {
        auto x = clean(dig, 5);
        for (size_t i = 0; i < 4000; i++) x[(size_t)(1.5 * kRate) + i] = cf32(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity());
        check("NaN and infinite samples", x);
    });
    jobs.add([=] { auto x = clean(dig, 5); impair::skip(x, 123457); impair::drop(x, (size_t)(1.7 * kRate), 31415); check("start mid-frame, samples lost", x); });
    jobs.add([=] {
        auto x = impair::clock(clean(dig, 4), 80); impair::shift(x, 5400, kRate); impair::echo(x, 20, -6, 2.0); impair::clip8(x, 2.0);
        check("combined: +5.4 kHz, +80 ppm, echo, 8 bit", x, 5400);
    });
    jobs.run();
    if (fails) { printf("%d check(s) failed\n", fails.load()); return 1; }
    printf("OK\n");
    return 0;
}
