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

static void rdsGood(const FmTelemetry& t, const char* what, float minOk) {
    CHECK(t.rdsSync && t.rdsBlockOkPct >= minOk, "%s: RDS blocks %.0f%% good (wanted %.0f%%)", what, t.rdsBlockOkPct, minOk);
    CHECK(t.psName == "TESTFM  " && t.radioText == "OnAir FM test signal" && t.piCode == 0x4A21, "%s: RDS contents", what);
}

int main() {
    FmGenConfig base;
    base.rate = 2e6;

    {   // one NaN sample in a file must not stop the receiver for good
        FmTelemetry t = run("NaN sample", base, 6, [](std::vector<cf32>& x) { x[1000000] = cf32(NAN, NAN); x[1000001] = cf32(INFINITY, 0); });
        CHECK(std::isfinite(t.cfoHz) && std::fabs(t.cfoHz) < 200, "NaN sample: carrier offset %f", t.cfoHz);
        CHECK(t.stereo && t.pilotPct > 7, "NaN sample: stereo lost");
        rdsGood(t, "NaN sample", 90);
    }
    return fails ? 1 : 0;
}
