// Threshold bench for SENSITIVITY-3.9-B (not built by CMake): build it by hand against dect2core; sens39b_bench navtex lo hi step trials [offHz driftHzPerMin], env LINES (message length), RATE (whole receiver at that input rate)
#include "../../tests/marine/navtex_bench.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "navtex";
    const double lo = argc > 2 ? atof(argv[2]) : 4, hi = argc > 3 ? atof(argv[3]) : 12, st = argc > 4 ? atof(argv[4]) : 1;
    const int trials = argc > 5 ? atoi(argv[5]) : 4;
    const double off = argc > 6 ? atof(argv[6]) : 0, drift = argc > 7 ? atof(argv[7]) : 0;
    if (!strcmp(mode, "navnoise")) {        // noise only: characters and messages the decoders report (false locks)
        for (int t = 0; t < trials; t++) {
            const auto r = sens39b::navtexRun(-60, 20, 900 + (uint32_t)t, 0, 0, 3.0, 6);
            printf("navtex noise only, %d: %llu characters, %d messages\n", t, (unsigned long long)r.chars, r.msgs);
        }
    }
    if (!strcmp(mode, "navtex")) {
        for (double s = hi; s >= lo - 1e-9; s -= st) {
            int sent = 0, ok = 0; double cpu = 0;
            for (int t = 0; t < trials; t++) {
                const auto r = sens39b::navtexRun(s, 4, 100 + (uint32_t)t * 31 + (uint32_t)(s * 10), off, drift, 3.0, getenv("LINES") ? atoi(getenv("LINES")) : 1, getenv("RATE") ? atof(getenv("RATE")) : 0);
                sent += r.sent; ok += r.clean; cpu += r.cpuPerSec;
            }
            printf("navtex snr %5.1f dB(300Hz) off %.0f drift %.0f/min: %d/%d clean (%.0f %%) cpu %.4f s/s\n", s, off, drift, ok, sent, 100.0 * ok / sent, cpu / trials);
            fflush(stdout);
        }
    }
    return 0;
}
