// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Red Pitaya has no analog gain: its gain control is a digital gain 0..+40 dB with an explanatory note; the Hermes Lite 2 keeps its LNA range.
#include "../../core/src/source/native_common.h"
#include <cstdio>
using namespace dect2;
int main() {
    int fails = 0;
    const DeviceInfo rp = native::redPitayaDevice("192.0.2.1", 1024);
    const DeviceInfo hl = native::hermesLite2Device("192.0.2.1", 1024);
    if (!(rp.gainMinDb == 0 && rp.gainMaxDb == 40)) { printf("FAIL rp range %g..%g\n", rp.gainMinDb, rp.gainMaxDb); fails++; }
    if (rp.gainNote.find("no analog") == std::string::npos) { printf("FAIL rp note\n"); fails++; }
    if (!(hl.gainMinDb == -12 && hl.gainMaxDb == 48 && hl.gainNote.empty())) { printf("FAIL hl2 unchanged\n"); fails++; }
    printf(fails ? "FAILED\n" : "redpitaya gain OK\n");
    return fails != 0;
}
