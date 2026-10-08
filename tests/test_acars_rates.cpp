// ACARS at the sample rates of the radios: 1 (the minimum), 2, 2.4, 8 and 10 Msps (HackRF exact rates, RTL-SDR), and a rate that is not a multiple of 100 kHz.
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
    for (double rate : {1e6, 2e6, 2.4e6, 2.048e6, 3.2e6, 8e6, 10e6}) {
        auto c = base(30);
        c.rate = rate;
        c.secs = rate > 5e6 ? 16 : 24;
        if (rate == 1e6) c.gen.channelsHz = {131.525e6, 131.725e6, 131.125e6};
        char w[48]; snprintf(w, sizeof w, "%.3f Msps", rate / 1e6);
        const auto r = scene(c, w, 0.98);
        printf("      receiver %.0f x real time\n", r.rtf);
    }
    printf(fails ? "acars rates: %d FAILED\n" : "acars rates: all passed\n", fails);
    return fails ? 1 : 0;
}
