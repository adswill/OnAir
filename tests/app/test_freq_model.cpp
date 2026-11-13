// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// app/state/freq_model.h: listening frequency, radio centre and hardware frequency, the readout's small line, and the SDR++ rule for a new
// listening frequency (the VFO inside the captured band, the radio only past its edge). The user report behind it: an Airspy HF+ at
// 0.384 Msps centred on 11.520 MHz listening to 11.710 MHz showed 11.520.000 in the big readout.
#include "../../app/state/freq_model.h"
#include <cmath>
#include <cstdio>
#include <string>

static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }

int main() {
    using namespace fmodel;
    // the report: dial 11.52 MHz, VFO +190 kHz, no converter
    Freqs f = compute(11.52e6, 190e3, 11.52e6, 11.52e6);
    check(std::fabs(f.listenHz - 11.71e6) < 0.5, "listening = dial + VFO offset");
    check(f.converterHz == 0, "no converter");
    check(groupHz(f.listenHz) == "11.710.000" && groupHz(f.centreHz) == "11.520.000", "grouped digits");
    check(secondaryLine(f, false) == "centre 11.520.000", "the small line names the centre");
    check(secondaryLine(f, true) == "listening 11.710.000", "with the centre in the big readout it names the listening frequency");
    // a converter: Ham It Up (+125 MHz); the hardware frequency is the centre through it
    f = compute(7.1e6, 0, 7.1e6, 132.1e6);
    check(std::fabs(f.converterHz - 125e6) < 1, "converter offset");
    check(secondaryLine(f, false) == "hardware 132.100.000 (converter +125 MHz)", "hardware line");
    f = compute(11.52e6, 0, 11.52e6, 17.52e6);
    check(secondaryLine(f, false) == "hardware 17.520.000 (converter +6 MHz)", "a +6 MHz converter");
    f = compute(10489.5e6, 0, 10489.5e6, 739.5e6);
    check(secondaryLine(f, false) == "hardware 739.500.000 (converter -9.75 GHz)", "an LNB below");
    check(secondaryLine(compute(100e6, 0, 100e6, 100e6), false).empty(), "nothing to say when all three agree");
    // SDR++ tuning: inside the band the VFO moves, the radio stays
    ListenPlan p = planListen(11.71e6, 11.52e6, 0.384e6, -5e3, 5e3);
    check(p.retune, "11.71 is outside 11.52 +- 0.45 * 0.384 MHz: the radio moves");
    p = planListen(11.60e6, 11.52e6, 0.384e6, -5e3, 5e3);
    check(!p.retune && p.centreHz == 11.52e6 && p.vfoHz == 11.60e6, "inside: only the VFO");
    p = planListen(11.6955e6, 11.52e6, 0.384e6, -5e3, 5e3);
    check(p.retune && p.centreHz == 11.6955e6, "the channel's edge past the band edge: retune onto it");
    p = planListen(12.0e6, 11.52e6, 0.384e6, 0, 0, true);
    check(!p.retune && std::fabs(p.vfoHz - (11.52e6 + 0.45 * 0.384e6)) < 1, "sticky: clamped into the band, no retune");
    p = planListen(100e6, 0, 0);
    check(p.retune, "no band known: retune");
    check(offsetText(-6e6) == "-6 MHz" && offsetText(125e6) == "+125 MHz" && offsetText(2.5e3) == "+2.5 kHz", "offset text");
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("freq_model: all ok\n");
    return 0;
}
