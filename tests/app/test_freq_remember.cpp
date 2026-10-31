// #393: ADS-B at 1090 MHz must not stay when switching to airband / VDL2 / AM (a value saved by 3.3.0 must not win over the default).
#include "../../app/state/freq_remember.h"
#include <cstdio>
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }
int main() {
    const double lo = 0.001, hi = 1e6;
    check(!freqrem::ok(lo, hi, 118.7, 1090), "airband: a saved 1090 is dropped");
    check(!freqrem::ok(lo, hi, 136.85, 1494), "VDL2: 1494 dropped");
    check(!freqrem::ok(lo, hi, 0.999, 1897), "AM: 1897 dropped");
    check(freqrem::ok(lo, hi, 118.7, 127.5), "airband: a normal remembered frequency stays");
    check(freqrem::ok(lo, hi, 1090, 1090), "ADS-B keeps 1090");
    check(freqrem::ok(lo, hi, 1090, 1030), "ADS-B user frequency in range stays");
    check(freqrem::ok(lo, hi, 1575.42, 1600), "GNSS keeps the user's frequency");
    check(freqrem::ok(lo, hi, 145.8, 3.5), "no clamp on a low frequency");
    check(!freqrem::ok(lo, hi, 100, 0), "never tuned (0) falls back");
    printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
