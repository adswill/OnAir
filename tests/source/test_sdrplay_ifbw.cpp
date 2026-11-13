// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SDRplay IF filter choice: a narrow channel tuned off the radio's centre must lie inside the filter (issue #199).
#include "../../core/src/source/sdrplay_api_min.h"
#include <cstdio>
using namespace dect2::sdrplay;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
int main() {
    CHECK(ifBandwidth(150e3, 0, 1e6, 0) == BW_0_200);          // no offset: as before
    CHECK(ifBandwidth(150e3, 0, 1e6, 200e3) == BW_0_600);      // Meteor LRPT: 2 * (200 + 75) = 550 kHz -> 600 kHz
    CHECK(ifBandwidth(150e3, 0, 1e6, 200e3 + 3.5e3) == BW_0_600);   // Doppler and oscillator error stay inside
    CHECK(ifBandwidth(12.5e3, 0, 2e6, 50e3) == BW_0_200);      // 50 kHz offset narrow channel: unchanged
    CHECK(ifBandwidth(150e3, 0, 250e3, 200e3) == BW_0_200);    // never wider than the sample rate
    CHECK(ifBandwidth(150e3, 1.75e6, 2e6, 200e3) == BW_1_536); // an explicit filter request is kept
    return failures ? 1 : 0;
}
