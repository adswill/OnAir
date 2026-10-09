// FM receiver against real-world faults (tests/impair.h, REAL_WORLD_CHECKLIST.md): the generator's signal is damaged independently of the receiver.
#include "dect2/fm_gen.h"
#include "dect2/fm_rx.h"
#include "impair.h"
#include <cstdio>
#include <functional>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static FmTelemetry run(const char* what, const FmGenConfig& c, double secs, const std::function<void(std::vector<cf32>&)>& fault) {
    FmGenerator g(c);
    std::vector<cf32> x;
    g.generate((size_t)(secs * c.rate), x);
    if (fault) fault(x);
    FmReceiver rx;
    rx.setSilent(true);
    rx.configure(c.rate);
    FmTelemetry t;
    uint64_t seq = 0;
    for (size_t i = 0; i < x.size(); i += 65536) {
        rx.feed(x.data() + i, std::min<size_t>(65536, x.size() - i));
        FmTelemetry u;
        if (rx.telemetry(u, seq)) { seq = u.seq; t = u; }
    }
    printf("%-34s state %d cfo %+.0f Hz stereo %d pilot %.1f%% | RDS sync %d ok %.0f%% groups %llu PS '%s' RT '%s' PI %04X\n", what, t.state, t.cfoHz, t.stereo,
           t.pilotPct, t.rdsSync, t.rdsBlockOkPct, (unsigned long long)t.rdsGroups, t.psName.c_str(), t.radioText.c_str(), t.piCode);
    return t;
}

// RDS sends 11.4 groups a second; secs: the length of the run (the first 1.5 s are allowed for locking)
static void rdsGood(const FmTelemetry& t, const char* what, float minOk, double secs) {
    CHECK(t.rdsSync && t.rdsBlockOkPct >= minOk, "%s: RDS blocks %.0f%% good (wanted %.0f%%)", what, t.rdsBlockOkPct, minOk);
    const double want = (secs - 1.5) * 1187.5 / 104 * 0.9;
    CHECK((double)t.rdsGroups >= want, "%s: %llu RDS groups (wanted %.0f)", what, (unsigned long long)t.rdsGroups, want);
    CHECK(t.psName == "TESTFM  " && t.radioText == "OnAir FM test signal" && t.piCode == 0x4A21, "%s: RDS contents", what);
}

int main() {
    FmGenConfig base;
    base.rate = 2e6;

    {   // one NaN sample in a file must not stop the receiver for good
        FmTelemetry t = run("NaN sample", base, 6, [](std::vector<cf32>& x) { x[1000000] = cf32(NAN, NAN); x[1000001] = cf32(INFINITY, 0); });
        CHECK(std::isfinite(t.cfoHz) && std::fabs(t.cfoHz) < 200, "NaN sample: carrier offset %f", t.cfoHz);
        CHECK(t.stereo && t.pilotPct > 7, "NaN sample: stereo lost");
        rdsGood(t, "NaN sample", 90, 6);
    }
    // the radio's sample clock off by +-100 ppm: the RDS bit clock (pilot / 16) then drifts against the receiver's 16 samples per bit
    for (double ppm : {-100.0, 100.0}) {
        char w[64]; snprintf(w, sizeof w, "sample clock %+.0f ppm", ppm);
        FmTelemetry t = run(w, base, 16, [ppm](std::vector<cf32>& x) { x = impair::clock(x, ppm); });
        rdsGood(t, w, 90, 8);
    }
    {   // a mono station with RDS (no pilot): RDS runs on its own 57 kHz carrier recovery
        FmGenConfig c = base; c.stereo = false;
        FmTelemetry t = run("mono with RDS", c, 8, nullptr);
        CHECK(!t.stereo, "mono with RDS: stereo reported");
        rdsGood(t, "mono with RDS", 90, 8);
    }
    // combined: the worst tuning error of the band (50 ppm of 108 MHz), the sample clock +80 ppm, an echo of 3 us at -8 dB and 8-bit clipping;
    // stereo and mono (whose RDS carrier is then 4.6 Hz off the receiver's 57 kHz)
    for (bool stereo : {true, false}) {
        FmGenConfig c = base; c.stereo = stereo;
        const char* w = stereo ? "combined, stereo" : "combined, mono";
        FmTelemetry t = run(w, c, 10, [](std::vector<cf32>& x) {
            impair::shift(x, 5400, 2e6); x = impair::clock(x, 80); impair::echo(x, 6, -8, 2.0); impair::clip8(x, 3); });
        CHECK(t.state == 2 && std::fabs(t.cfoHz - 5400) < 300, "%s: state %d, offset %+.0f Hz", w, t.state, t.cfoHz);
        CHECK(t.stereo == stereo, "%s: stereo %d", w, t.stereo);
        rdsGood(t, w, 90, 10);
    }
    return fails ? 1 : 0;
}
