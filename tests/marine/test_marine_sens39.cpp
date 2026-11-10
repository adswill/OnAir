// NAVTEX sensitivity (docs/SENSITIVITY-3.9-B.md): messages that arrive complete with every character right, at low SNR (noise in 300 Hz),
// with a carrier offset and drift, through the real FSK slicer, centre finder and decoder; noise alone must give no characters; the
// whole receiver must decode an input whose noise is far above 8 units (the old fixed-point clip of the front end).
// Default: a few points (seconds). ONAIR_TEST_LONG=1: the full threshold sweep. Simulated signals only.
#include "navtex_bench.h"
#include <cstdio>
#include <cstdlib>

static int fails = 0;
static void check(bool ok, const char* what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

int main() {
    using namespace sens39b;
    // 4 dB in 300 Hz: before 3.9 about 70 % (short messages) / 29 % (long); now 98 % / 92 %
    {
        int ok = 0, sent = 0;
        for (int t = 0; t < 3; t++) { const auto r = navtexRun(4.0, 4, 100 + (uint32_t)t * 31 + 40); ok += r.clean; sent += r.sent; }
        printf("4 dB, short messages: %d/%d\n", ok, sent);
        check(ok >= sent - 1, "4 dB: all but one message clean");
    }
    {
        const auto r = navtexRun(5.0, 3, 7, 250.0, 0, 3.0, 6);
        printf("5 dB, +250 Hz, long messages: %d/%d\n", r.clean, r.sent);
        check(r.clean >= 2, "5 dB with a 250 Hz carrier offset");
    }
    {
        const auto r = navtexRun(5.0, 3, 8, -150.0, 60.0, 3.0, 6);
        printf("5 dB, -150 Hz drifting 60 Hz/min: %d/%d\n", r.clean, r.sent);
        check(r.clean >= 2, "5 dB with drift");
    }
    {
        const auto r = navtexRun(-60.0, 6, 900, 0, 0, 3.0, 6);
        printf("noise only: %llu characters, %d messages\n", (unsigned long long)r.chars, r.msgs);
        check(r.chars == 0 && r.msgs == 0, "noise only: nothing decoded");
    }
    {
        // whole receiver at 250 kHz, input scaled by 4 (a float recording): the noise is about 45 units rms per rail at 6 dB, the old
        // front end clipped every rail at 8 and decoded nothing
        const auto r = navtexRun(6.0, 2, 11, 0, 0, 3.0, 2, 250000.0, 4.f);
        printf("receiver at 250 kHz, 6 dB: %d/%d\n", r.clean, r.sent);
        check(r.clean == r.sent, "receiver: input far over 8 units is not clipped");
    }
    if (getenv("ONAIR_TEST_LONG")) {
        for (int lines : {1, 6})
            for (double s = 8; s >= 1.5; s -= 1) {
                int ok = 0, sent = 0;
                for (int t = 0; t < 10; t++) { const auto r = navtexRun(s, 4, 100 + (uint32_t)t * 31 + (uint32_t)(s * 10), 0, 0, 3.0, lines); ok += r.clean; sent += r.sent; }
                printf("sweep lines %d snr %.0f dB: %d/%d (%.0f %%)\n", lines, s, ok, sent, 100.0 * ok / sent);
            }
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
