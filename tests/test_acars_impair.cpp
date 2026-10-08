// ACARS with the faults of real radios: carrier offset, clock offset, 8-bit samples, DC offset, a gap, a reset, a weak channel next to a strong one.
#include "dect2/acars_sim.h"
#include <cstdio>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
static AcarsSimCfg base(double snr = 35) {
    AcarsSimCfg c;
    c.syn.snrDb = snr;
    c.gen.rateFactor = 4;
    c.secs = 28;
    return c;
}
// runs one scene, prints one line, and asks for at least minFrac of the blocks with nothing wrong
static AcarsSimResult scene(AcarsSimCfg c, const char* what, double minFrac) {
    const auto r = runAcarsSim(c);
    const double f = r.sent ? (double)r.decoded / r.sent : 0;
    printf("  %-40s sent %3d  decoded %3d (%5.1f %%)  wrong %d  failed checks %llu\n", what, r.sent, r.decoded, 100 * f, r.wrong, (unsigned long long)r.last.blocksBad);
    CHECK(r.sent >= 30, "%s: only %d blocks sent", what, r.sent);
    CHECK(f >= minFrac, "%s: %.1f %% decoded, wanted %.1f %%", what, 100 * f, 100 * minFrac);
    CHECK(r.wrong == 0, "%s: %d wrong messages let through", what, r.wrong);
    return r;
}

int main() {
    printf("carrier offset (every channel shifted; 10 ppm of 131.5 MHz is 1.3 kHz)\n");
    for (double cfo : {-3000.0, -1300.0, 1300.0, 3000.0}) {
        auto c = base(30); c.syn.cfoHz = cfo;
        char w[48]; snprintf(w, sizeof w, "carrier offset %+.0f Hz", cfo);
        scene(c, w, 0.98);
    }
    printf("sample clock offset\n");
    for (double ppm : {-200.0, -50.0, 50.0, 200.0}) {
        auto c = base(30); c.syn.sroPpm = ppm;
        char w[48]; snprintf(w, sizeof w, "clock offset %+.0f ppm", ppm);
        scene(c, w, 0.98);
    }
    printf("8-bit samples, DC offset\n");
    { auto c = base(30); c.quant8 = true; scene(c, "8-bit samples", 0.98); }
    { auto c = base(30); c.quant8 = true; c.dc = cf32(0.06f, -0.05f); scene(c, "8-bit samples, DC 0.06 / -0.05", 0.98); }
    printf("a gap of 20 ms, and a reset, in the middle\n");
    { auto c = base(30); c.gapAt = 13.37; c.gapSec = 0.020; const auto r = scene(c, "20 ms gap at 13.37 s", 0.98); CHECK(r.last.blocksOk > 40, "decoding goes on after the gap"); }
    { auto c = base(30); c.resetAt = 12.0; const auto r = scene(c, "reset() at 12 s", 0.98); CHECK(r.msgs.size() > 20, "messages after the reset: %zu", r.msgs.size()); }
    printf("a weak channel 25 kHz from a strong one\n");
    for (double diff : {20.0, 30.0, 40.0}) {
        auto c = base(diff + 28);
        c.gen.channelsHz = {131.525e6, 131.550e6};
        c.gen.levelDb = {0.0, -diff};
        c.gen.aircraft = 6;
        char w[56]; snprintf(w, sizeof w, "neighbour %.0f dB weaker", diff);
        const auto r = scene(c, w, 0.98);
        int weak = 0, strong = 0;
        for (const auto& m : r.msgs) (m.freqHz > 131.54e6 ? weak : strong)++;
        CHECK(weak >= 10 && strong >= 10, "messages: %d on the weak channel, %d on the strong one", weak, strong);
    }
    printf("bursts that overlap in time on different channels (eight busy channels)\n");
    {
        auto c = base(30);
        c.gen.channelsHz = {131.125e6, 131.150e6, 131.525e6, 131.550e6, 131.725e6, 131.825e6, 131.850e6, 131.875e6};
        c.gen.aircraft = 24; c.gen.rateFactor = 6;
        scene(c, "eight channels, 24 aircraft", 0.98);
    }
    printf(fails ? "acars impair: %d FAILED\n" : "acars impair: all passed\n", fails);
    return fails ? 1 : 0;
}
