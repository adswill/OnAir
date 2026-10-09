// DAB against the faults of REAL_WORLD_CHECKLIST.md: the generator's clean signal goes through tests/impair.h (written independently of
// the receiver) and then through the real DabReceiver. Band III radios are up to 12 kHz off, L-band ones up to 75 kHz; sample clocks up to
// 100 ppm; swapped I/Q; lost samples; NaN samples; and one combined case (L-band offset, +80 ppm, an echo, 8-bit clipping).
#include "dect2/dab.h"
#include "dect2/dab_gen.h"
#include "impair.h"
#include "jobs.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>
using namespace dect2;
using testjobs::jprintf;

static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL line %d: ", __LINE__); jprintf(__VA_ARGS__); jprintf("\n"); fails++; } } while (0)

static const double kRate = 2.048e6;

static std::vector<cf32> clean(double seconds) {
    dabgen::TxConfig tc;
    tc.utcSeconds = 1700000000;
    SynthConfig sc;
    sc.snrDb = 40;
    auto syn = makeDabSynth(tc, sc, kRate);
    std::vector<cf32> x((size_t)(seconds * kRate));
    syn->generate(x.data(), x.size());
    return x;
}

struct Res { uint64_t fibOk = 0, fibBad = 0, auOk = 0, auBad = 0, frames = 0; double cfo = 0; int state = 0; size_t services = 0; };

static Res run(const std::vector<cf32>& x) {
    DabReceiver rx;
    rx.configure(kRate);
    rx.audio().setSilent(true);
    rx.select(1);
    for (size_t i = 0; i < x.size(); i += 32768) rx.feed(x.data() + i, std::min<size_t>(32768, x.size() - i));
    DabTelemetry t;
    rx.telemetry(t, 0);
    Res r;
    r.fibOk = t.fibOk; r.fibBad = t.fibBad; r.frames = t.frames; r.cfo = t.cfoHz; r.state = t.state;
    r.auOk = rx.audio().stats().auOk; r.auBad = rx.audio().stats().auBad;
    r.services = rx.ensemble().services.size();
    return r;
}

static void expect(const char* what, const Res& r, double minFib, double cfo = std::numeric_limits<double>::quiet_NaN()) {
    const double n = (double)(r.fibOk + r.fibBad);
    const double fib = n > 0 ? (double)r.fibOk / n : 0;
    jprintf("%-40s locked %d, frames %llu, FIB ok %.2f%%, CFO %+.0f Hz, AU %llu ok %llu bad, %zu services\n", what, r.state == 2, (unsigned long long)r.frames, 100 * fib, r.cfo,
            (unsigned long long)r.auOk, (unsigned long long)r.auBad, r.services);
    CHECK(r.state == 2 && fib >= minFib && r.services == 4 && r.auOk > 50 && r.auBad * 20 <= r.auOk, "%s", what);
    if (!std::isnan(cfo)) CHECK(std::fabs(r.cfo - cfo) < 50, "%s: CFO read %.0f, sent %.0f", what, r.cfo, cfo);
}

int main() {
    const std::vector<cf32> base = clean(8.0);
    testjobs::Jobs jobs;
    for (double hz : {12000.0, -12000.0, 40500.0, 75000.0, -75000.0}) jobs.add([&, hz] {
        auto x = base; impair::shift(x, hz, kRate);
        char b[64]; snprintf(b, sizeof b, "tuning error %+.0f Hz", hz);
        expect(b, run(x), 0.99, hz);
    });
    for (double ppm : {-100.0, -80.0, 80.0, 100.0}) jobs.add([&, ppm] {
        auto x = impair::clock(base, ppm); impair::shift(x, 3000, kRate);
        char b[64]; snprintf(b, sizeof b, "sample clock %+.0f ppm", ppm);
        expect(b, run(x), 0.99);
    });
    jobs.add([&] { auto x = base; impair::shift(x, 2300, kRate); impair::swapIq(x); expect("swapped I/Q", run(x), 0.99); });
    jobs.add([&] { auto x = base; impair::dc(x, 0); expect("DC spike at the signal's level", run(x), 0.99); });
    jobs.add([&] {
        auto x = base;
        for (size_t i = 0; i < 4000; i++) x[3000000 + i] = cf32(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity());
        expect("NaN and infinite samples", run(x), 0.95);
    });
    jobs.add([&] { auto x = base; impair::drop(x, 2500000, 12345); impair::drop(x, 9000000, 777); impair::skip(x, 100000); expect("lost samples, start mid-frame", run(x), 0.95); });
    jobs.add([&] {
        auto x = impair::clock(base, 80); impair::shift(x, 75000, kRate); impair::echo(x, 300, -6, 2.0); impair::clip8(x, 2.5);
        expect("combined: +75 kHz, +80 ppm, echo, 8 bit", run(x), 0.97, 75000);
    });
    jobs.run();
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
