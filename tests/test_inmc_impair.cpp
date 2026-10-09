// Inmarsat-C receiver against the impairments of real radios: carrier offset over +-10 ppm of 1.5 GHz and beyond, carrier drift, clock error,
// every chunk size, 8-bit samples, DC offset, a gap, a reset in the middle of the stream. Eb/N0 8 dB unless a case says otherwise.
#include "dect2/inmc_testutil.h"
#include "impair.h"
#include "dect2/test_parallel.h"
#include <algorithm>
#include <functional>
#include <numeric>
using namespace dect2;
using dect2::testpar::CaseOut;
// every case runs on its own thread and prints through `out`, so that the log keeps the order of the cases
#define CHECK(c, ...) do { if (!(c)) out.fail(__LINE__, __VA_ARGS__); } while (0)

static InmcRunResult run(CaseOut& out, const char* name, InmcRunOpts o, int minGood, int maxBad = 0) {
    const InmcRunResult r = runInmc(o);
    const InmcTelemetry& t = r.t;
    out.print("  %-34s sync %.1f s, %llu good, %llu bad frames, %d messages complete, cfo %+.1f Hz, drift %+.1f Hz/s, Eb/N0 %.1f dB, %.0fx real time\n", name,
              r.firstSyncSec, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, r.completeMatches, t.cfoHz, t.driftHzS, t.ebn0Db, r.rtf);
    CHECK(t.blocksOk >= (uint64_t)minGood, "%s: %llu good frames, wanted %d", name, (unsigned long long)t.blocksOk, minGood);
    CHECK(t.blocksBad <= (uint64_t)maxBad, "%s: %llu bad frames", name, (unsigned long long)t.blocksBad);
    return r;
}

static InmcRunOpts base(double secs = 40) {
    InmcRunOpts o;
    o.gen.rate = 2e6; o.gen.ebn0Db = 8; o.gen.seed = 2; o.secs = secs;
    return o;
}

int main() {
    // every run is a job of its own (they share nothing): four at a time, the log keeps the order of the list
    std::vector<std::function<void(CaseOut&)>> jobs;
    std::vector<double> cost;       // signal seconds times sample rate, to start the biggest first
    auto add = [&](double secs, double rate, std::function<void(CaseOut&)> f) { cost.push_back(secs * rate); jobs.push_back(std::move(f)); };
    // carrier offset: +-10 ppm of 1537 MHz is 15.4 kHz; the search covers +-20 kHz
    for (double cfo : {0.0, 15400.0, -15400.0, 19000.0, -19000.0, 30000.0, 77000.0, -77000.0})   // 50 ppm of 1537 MHz is 77 kHz
        add(40, 2e6, [cfo](CaseOut& out) {
            InmcRunOpts o = base(std::fabs(cfo) > 20000 ? 50 : 40);   // beyond the channel's own search the band search moves it there first
            o.gen.cfoHz = cfo;
            char n[64];
            snprintf(n, sizeof n, "carrier offset %+.0f Hz", cfo);
            const InmcRunResult r = run(out, n, o, 3);
            CHECK(std::fabs(r.t.cfoHz - cfo) < 3, "%s: read %+.1f Hz", n, r.t.cfoHz);
        });
    // combined (REAL_WORLD_CHECKLIST.md): 50 ppm, a sample clock 80 ppm fast, an echo, 8-bit clipping
    add(50, 2e6, [](CaseOut& out) {
        InmcRunOpts o = base(50);
        o.gen.cfoHz = -77000; o.gen.sroPpm = 80; o.quantize8 = true;
        o.mod = [](cf32* x, size_t n) { std::vector<cf32> v(x, x + n); impair::echo(v, 4, -8, 2.0); impair::clip8(v, 2.0); std::copy(v.begin(), v.end(), x); };
        const InmcRunResult r = run(out, "combined -77 kHz +80 ppm echo 8 bit", o, 3);
        CHECK(std::fabs(r.t.cfoHz + 77000) < 20, "combined: read %+.1f Hz", r.t.cfoHz);
    });
    // drift: 20 Hz/s over 55 s moves the carrier by 1.1 kHz
    add(55, 2e6, [](CaseOut& out) {
        InmcRunOpts o = base(55);
        o.gen.cfoHz = 5000; o.gen.driftHzS = 20;
        const InmcRunResult r = run(out, "drift 20 Hz/s", o, 4);
        CHECK(std::fabs(r.t.driftHzS - 20) < 6, "drift read %+.1f Hz/s", r.t.driftHzS);
        CHECK(std::fabs(r.t.cfoHz - (5000 + 20 * 55)) < 80, "carrier at the end %+.0f Hz, expected about %.0f", r.t.cfoHz, 5000 + 20 * 55.0);
    });
    add(55, 2e6, [](CaseOut& out) {
        InmcRunOpts o = base(55);
        o.gen.cfoHz = 5000; o.gen.driftHzS = -20;
        run(out, "drift -20 Hz/s", o, 4);
    });
    // sample clock error
    for (double ppm : {50.0, -50.0})
        add(45, 2e6, [ppm](CaseOut& out) {
            InmcRunOpts o = base(45);
            o.gen.sroPpm = ppm; o.gen.cfoHz = 2000;
            char n[64];
            snprintf(n, sizeof n, "clock error %+.0f ppm", ppm);
            run(out, n, o, 4);
        });
    // chunk sizes (1 and 7 at the minimum rate of 250 ksps, where a million calls are cheap)
    for (size_t chunk : {(size_t)1, (size_t)7, (size_t)65536})
        add(30, 250000 * (chunk < 100 ? 8 : 1), [chunk](CaseOut& out) {
            InmcRunOpts o = base(30);
            o.gen.rate = 250000;
            o.chunk = chunk;
            char n[64];
            snprintf(n, sizeof n, "chunk %zu at 250 ksps", chunk);
            run(out, n, o, 2);
        });
    add(40, 2e6, [](CaseOut& out) {
        InmcRunOpts o = base(40);
        o.chunk = 65536;
        run(out, "chunk 65536 at 2 Msps", o, 3);
    });
    // 8 bit samples, DC offset, gap, reset
    add(40, 2e6, [](CaseOut& out) {
        InmcRunOpts o = base();
        o.quantize8 = true;
        run(out, "8-bit samples", o, 3);
    });
    add(40, 2e6, [](CaseOut& out) {
        InmcRunOpts o = base();
        o.quantize8 = true;
        o.dc = 0.08f;
        run(out, "8-bit and DC offset 8 %", o, 3);
    });
    add(60, 2e6, [](CaseOut& out) {
        InmcRunOpts o = base(60);
        o.gapAt = 20.0; o.gapLen = 0.020;
        run(out, "20 ms gap at 20 s", o, 5, 1);
    });
    add(70, 2e6, [](CaseOut& out) {
        InmcRunOpts o = base(70);
        o.resetAt = 25.0;
        const InmcRunResult r = run(out, "reset at 25 s", o, 3);
        CHECK(r.t.state == 2, "state after reset %d", r.t.state);
    });
    // polarity flips inside a frame: the carrier loop has two stable points 180 degrees apart and a slip moves it to the other one
    add(60, 2e6, [](CaseOut& out) {
        InmcRunOpts o = base(60);
        o.flipAt = 21.0;
        const InmcRunResult r = run(out, "polarity flip at 21 s", o, 5);
        CHECK(r.t.polaritySlips >= 1, "no slip repaired (%llu)", (unsigned long long)r.t.polaritySlips);
    });
    add(60, 2e6, [](CaseOut& out) {
        InmcRunOpts o = base(60);
        o.flipAt = 21.0;
        o.flipBack = 40.0;
        const InmcRunResult r2 = run(out, "flip at 21 s, back at 40 s", o, 5);
        CHECK(r2.t.polaritySlips >= 2, "slips repaired %llu, wanted 2", (unsigned long long)r2.t.polaritySlips);
        CHECK(r2.completeMatches >= 3, "%d messages complete after the flips", r2.completeMatches);
    });
    std::vector<size_t> order(jobs.size());
    std::iota(order.begin(), order.end(), (size_t)0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return cost[a] > cost[b]; });
    const int fails = dect2::testpar::runCases(jobs.size(), [&](size_t i, CaseOut& out) { jobs[i](out); }, order);
    printf(fails ? "FAILED\n" : "inmc_impair ok\n");
    return fails ? 1 : 0;
}
