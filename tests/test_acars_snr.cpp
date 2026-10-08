// ACARS sensitivity: decode rate against carrier-to-noise ratio (7 kHz band), and against AM depth.
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
    printf("signal-to-noise sweep (carrier over noise in 7 kHz), three channels, 2 Msps\n");
    double at90 = -1;
    double prev = 1;
    for (double snr : {24.0, 18.0, 16.0, 14.0, 13.0, 12.0, 11.0, 10.0, 9.0}) {
        auto c = base(snr);
        c.gen.seed = 3;
        char w[48]; snprintf(w, sizeof w, "SNR %.0f dB", snr);
        const auto r = scene(c, w, 0.0);
        const double f = r.sent ? (double)r.decoded / r.sent : 0;
        if (f >= 0.9) at90 = snr;
        CHECK(f <= prev + 0.05, "decode rate should fall with the SNR: %.2f after %.2f", f, prev);
        prev = f;
        if (snr >= 16) CHECK(f >= 0.98, "SNR %.0f dB: %.1f %%", snr, 100 * f);
        if (snr == 13) CHECK(f >= 0.90, "SNR 13 dB: %.1f %%", 100 * f);
    }
    printf("  90 %% of the blocks decode down to %.0f dB\n", at90);
    CHECK(at90 >= 0 && at90 <= 13, "90 %% point %.0f dB", at90);
    printf("AM depth, 30 dB\n");
    for (double d : {0.3, 0.5, 0.7, 0.9}) {
        auto c = base(30);
        c.gen.depth = d;
        char w[48]; snprintf(w, sizeof w, "depth %.0f %%", d * 100);
        scene(c, w, 0.98);
    }
    printf(fails ? "acars snr: %d FAILED\n" : "acars snr: all passed\n", fails);
    return fails ? 1 : 0;
}
