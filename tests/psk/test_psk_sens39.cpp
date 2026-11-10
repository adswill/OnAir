// BPSK31 sensitivity (docs/SENSITIVITY-3.9-B.md): words of a three-line over decoded at 9 dB in the baud bandwidth (Eb/N0 9 dB), plain and with a
// 20 Hz tuning error drifting 30 Hz/min, through the whole receiver. With these 3 trials: 76 % and 64 % before the phase-reference decision (3.9), 88 % and 88 % after.
// ONAIR_TEST_LONG=1: the sweep 10 .. 6 dB. Simulated signals only.
#include "psk_bench.h"
#include <cstdio>
#include <cstdlib>

static int fails = 0;
static void check(bool ok, const char* what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

static double share(double snr, int trials, double cfo, double drift) {
    int w = 0, f = 0;
    for (int t = 0; t < trials; t++) { const auto r = sens39b::pskRun(snr, 10 + (uint32_t)t * 7 + (uint32_t)(snr * 10 + 500), 0, 1000, cfo, drift); w += r.words; f += r.found; }
    return w ? (double)f / w : 0;
}

int main() {
    const double a = share(9, 3, 0, 0), b = share(9, 3, 20, 30);
    printf("9 dB: %.0f %% of the words, with 20 Hz drifting 30 Hz/min: %.0f %%\n", 100 * a, 100 * b);
    check(a >= 0.85, "9 dB: most words");
    check(b >= 0.80, "9 dB with tuning error and drift");
    if (getenv("ONAIR_TEST_LONG"))
        for (double s = 10; s >= 6; s -= 1) printf("sweep %.0f dB: %.1f %%\n", s, 100 * share(s, 8, 0, 0));
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
