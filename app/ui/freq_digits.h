// The arithmetic of the SDR++ style frequency readout (ui2_widgets.cpp freqDigitsImpl): thirteen digits, each one a place of ten, changed by a
// click (upper half up, lower half down), the wheel (a trackpad's small values add up to whole steps) or typing a number over it.
// No ImGui here, so tests/general40/test_general_layout.cpp checks it.
#pragma once
#include <algorithm>

namespace fdigits {

constexpr int kDigits = 13;   // up to 1000 GHz (10^12 Hz)

// the place of digit i (0 = the leftmost): 10^12 ... 1
inline long long placeOf(int i) {
    long long p = 1;
    for (int k = 0; k < kDigits - 1 - i; k++) p *= 10;
    return p;
}
inline int digitAt(long long hz, int i) { return (int)((hz / placeOf(i)) % 10); }

// the change that makes digit i read k (typing k over it): the other digits stay
inline long long typeDelta(long long hz, int i, int k) { return (long long)(k - digitAt(hz, i)) * placeOf(i); }

// wheel input: a mouse notch is 1.0; a trackpad sends small values that add up. Returns the whole steps now and keeps the rest in acc.
inline int wheelSteps(float& acc, float wheel) {
    acc += wheel;
    const int s = (int)acc;
    acc -= (float)s;
    return s;
}

// the frequency after a change of delta Hz, in MHz, kept inside [loMhz, hiMhz] (a step past 0 or past the top stops there)
inline double applyMhz(long long hz, long long delta, double loMhz, double hiMhz) { return std::min(hiMhz, std::max(loMhz, (double)(hz + delta) / 1e6)); }

}  // namespace fdigits
