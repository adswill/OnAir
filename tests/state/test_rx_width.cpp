// The custom-bandwidth plan (#274, 3.4): the standard changes nothing, any width gives a rate that carries it, every mode takes it, nothing is clamped.
#include "../../app/state/rx_width.h"
#include <cmath>
#include <cstdio>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main() {
    RxWidthPlan a = rxWidthPlan(0, 1.8, 2e6, 1.75e6, 2e5);
    CHECK(a.bandwidthMhz == 1.8 && a.sampleRate == 2e6 && a.basebandHz == 1.75e6);   // Auto: the mode's own values
    RxWidthPlan n = rxWidthPlan(-3, 0.4, 2e6, 1.75e6, 250000);
    CHECK(n.sampleRate == 2e6);
    RxWidthPlan w = rxWidthPlan(2.0, 1.8, 2e6, 1.75e6, 2e5);
    CHECK(w.bandwidthMhz == 2.0 && w.sampleRate == 2.25e6 && w.sampleRate >= w.bandwidthMhz * 1e6 && w.basebandHz < w.sampleRate);
    RxWidthPlan big = rxWidthPlan(9, 1.8, 2e6, 1.75e6, 2e5);
    CHECK(big.bandwidthMhz == 9 && big.sampleRate == 10e6 && big.basebandHz == 9e6);  // 3.4: no 2 MHz limit
    RxWidthPlan huge = rxWidthPlan(20, 1.8, 2e6, 1.75e6, 2e5);
    CHECK(huge.bandwidthMhz == 20 && huge.sampleRate == 22.25e6 && huge.sampleRate > huge.bandwidthMhz * 1e6);   // the whole 380 - 400 MHz TETRA range: the radio is asked for it, the driver decides
    RxWidthPlan s = rxWidthPlan(0.01, 0.2, 250000, 200000, 64000);
    CHECK(s.bandwidthMhz == 0.01 && s.sampleRate == 250000 * 1.0 * 1 && s.sampleRate >= 64000);   // no 0.05 MHz minimum; 0.01 / 0.9 -> 0.25 Msps
    RxWidthPlan floor = rxWidthPlan(0.25, 0.4, 2e6, 1.75e6, 800000);
    CHECK(floor.sampleRate == 800000);                                               // never below what the mode needs
    CHECK(rxWidthPlan(NAN, 1.8, 2e6, 1.75e6, 2e5).sampleRate == 2e6 && rxWidthPlan(INFINITY, 1.8, 2e6, 1.75e6, 2e5).bandwidthMhz == 1.8);   // not a width: the standard
    CHECK(parseWidthMhz("1.5") == 1.5 && parseWidthMhz("250k") == 0.25 && parseWidthMhz("12 MHz") == 12 && parseWidthMhz("12.5 kHz") == 0.0125 && parseWidthMhz("5000 Hz") == 0.005);
    CHECK(parseWidthMhz("") == 0 && parseWidthMhz("abc") == 0 && parseWidthMhz("-3") == 0 && parseWidthMhz(nullptr) == 0);
    CHECK(!bwCustomTicked(-1, 0) && bwCustomTicked(-1, 0.5) && bwCustomTicked(-1, 2.0));   // never saved: a saved width keeps working, Auto stays standard
    CHECK(bwCustomTicked(1, 0) && !bwCustomTicked(0, 0.5) && bwCustomTicked(1, 0.5));      // a saved tick wins over the width
    CHECK(bwEffective(true, 0.5) == 0.5 && bwEffective(false, 0.5) == 0 && bwEffective(true, 0) == 0);   // unticked: the standard value, the width is kept for later
    std::printf(fails ? "rx_width FAILED\n" : "rx_width ok\n");
    return fails ? 1 : 0;
}
