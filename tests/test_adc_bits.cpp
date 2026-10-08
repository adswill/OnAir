// What counts as a low ADC level follows the radio's converter (gain.h setAdcBits): an 8-bit radio at -30 dBFS rms is "low", a 12-bit
// one is fine there, and the gain loop no longer climbs on a 12-bit radio that sits at a healthy -30 dBFS.
#include "dect2/gain.h"
#include <cstdio>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static bool climbs(double rms) {
    AutoGain agc; agc.setGenericMax(100);
    GainSetting g; g.vga = 40;
    SignalStats st; st.rmsDbfs = (float)rms; st.peak = 0.2f; st.clipFraction = 0;
    bool changed = false;
    for (double t = 0; t < 10; t += 0.5) { GainSetting before = g; if (agc.update(t, st, g) && g.total() > before.total()) changed = true; }
    return changed;
}

int main() {
    setAdcBits(8);
    CHECK(classifyAdc(-30, 0.1, 0) == AdcStatus::Low, "8 bits: -30 dBFS is low");
    CHECK(classifyAdc(-16, 0.3, 0) == AdcStatus::Good, "8 bits: -16 dBFS is good");
    CHECK(climbs(-30), "8 bits: the gain loop raises the gain at -30 dBFS");
    setAdcBits(12);
    CHECK(classifyAdc(-30, 0.1, 0) == AdcStatus::Good, "12 bits: -30 dBFS is good");
    CHECK(classifyAdc(-48, 0.01, 0) == AdcStatus::Low, "12 bits: -48 dBFS is low");
    CHECK(classifyAdc(-5, 0.6, 0) == AdcStatus::High, "12 bits: -5 dBFS is still high");
    CHECK(classifyAdc(-20, 1.0, 0.01) == AdcStatus::Overload, "12 bits: clipping is still overload");
    CHECK(!climbs(-30), "12 bits: no climbing at a healthy -30 dBFS");
    CHECK(climbs(-49), "12 bits: climbing at -49 dBFS");
    setAdcBits(8);
    printf(fails ? "adc bits: FAILED\n" : "adc bits: ok\n");
    return fails ? 1 : 0;
}
