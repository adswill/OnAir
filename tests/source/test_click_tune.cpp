// Click on the spectrum -> dial: the clicked signal is the dial, the radio sits the mode's offset above it (issue #219).
#include "dect2/engine/click_tune.h"
#include <cmath>
#include <initializer_list>
#include <cstdio>
using namespace dect2;
int main() {
    int f = 0;
    for (double c : {434.010, 434.200, 433.920}) {
        const double dial = dialMhzFromClick(c, 250000);
        const double radio = radioMhzForDial(dial, 250000);
        if (std::fabs(dial - c) > 1e-9 || std::fabs(radio - (c + 0.25)) > 1e-9) { printf("FAIL click %.3f dial %.3f radio %.3f\n", c, dial, radio); f++; }
    }
    return f;
}
