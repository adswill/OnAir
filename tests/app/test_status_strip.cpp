// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Status bar helpers: frequency digit split and grouping, CPU level thresholds, and which segments stay at many window widths.
#include "../../app/ui/status_strip.h"
#include <cstdio>
using namespace statusstrip;
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }
int main() {
    check(splitFreq(851.013).lead == "851" && splitFreq(851.013).rest == ".013", "851.013");
    check(splitFreq(93.9).lead == "93.9" && splitFreq(93.9).rest == "00", "93.900 keeps three digits in the accent");
    check(splitFreq(1090.0).lead == "109" && splitFreq(1090.0).rest == "0.000", "1090");
    check(splitFreq(0.5).lead == "0.50" && splitFreq(0.5).rest == "0", "0.500");
    check(splitFreq(-3).lead + splitFreq(-3).rest == "0.000", "negative clamps");
    check(cpuLevel(0) == 0 && cpuLevel(150) == 0 && cpuLevel(150.1f) == 1 && cpuLevel(250) == 1 && cpuLevel(250.1f) == 2, "cpu thresholds");
    check(cpuFill(-5, 4) == 0 && cpuFill(200, 4) == 0.5f && cpuFill(900, 4) == 1.f && cpuFill(50, 0) == 0.5f, "cpu fill");
    check(groupedMhz(522.0) == "522.000 000", "522 MHz grouped");
    check(groupedMhz(1090.0) == "1 090.000 000", "1090 MHz grouped on both sides");
    check(groupedMhz(93.9) == "93.900 000", "93.9");
    check(groupedMhz(0.0001) == "0.000 100", "100 Hz");
    check(groupedMhz(10489.5505) == "10 489.550 500", "QO-100 downlink");
    check(groupedMhz(-1) == "0.000 000", "negative clamps");
    // the flat bar: state 0, frequency 0, mode 1, rx 5, rate 3, cpu 2, clock 4 (widths in px)
    std::vector<Seg> v = {{80, 0}, {130, 0}, {60, 1}, {40, 5}, {90, 3}, {70, 2}, {60, 4}};
    const float gap = 13;
    check(fit(v, 2000, gap) == usedWidth(v, gap), "fit reports the width used");
    bool all = true; for (const Seg& s : v) all = all && s.on;
    check(all, "wide shows every segment");
    int lastOn = (int)v.size();
    for (float avail = 700; avail >= 0; avail -= 7) {
        const float used = fit(v, avail, gap);
        int on = 0; for (const Seg& s : v) on += s.on;
        check(on <= lastOn, "segments only drop as the bar narrows");
        lastOn = on;
        check(v[0].on && v[1].on, "state and frequency always stay");
        if (used > avail) check(on == 2, "only the priority-0 segments may overflow");
        for (size_t i = 0; i < v.size(); i++) for (size_t j = 0; j < v.size(); j++)
            if (!v[i].on && v[j].on) check(v[j].prio <= v[i].prio, "nothing more important is dropped before something less important");
    }
    // equal priorities: the rightmost goes first
    std::vector<Seg> t = {{50, 0}, {50, 1}, {50, 1}, {50, 1}};
    fit(t, 50 * 3 + 2 * gap, gap);
    check(t[1].on && t[2].on && !t[3].on, "ties drop from the right");
    if (!fails) printf("ok\n");
    return fails;
}
