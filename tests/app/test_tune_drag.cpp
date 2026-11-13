// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "../../app/gfx/tune_drag.h"
#include <cstdio>
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }
int main() {
    using namespace tunedrag;
    check(!isDrag(3, 3, 0.1), "small move is a click");
    check(isDrag(4, 0, 0.1), "4 px is a drag");
    check(!isDrag(0, 0, 2.0), "slow still press is a click");
    check(clampCentre(2000, 1, 1766) == 1766 && clampCentre(0, 1, 1766) == 1, "clamp");
    Pacer p;
    check(p.step(0.5, 1.0) == 0.5, "first step passes");
    check(p.step(0.2, 1.01) == 0 && p.step(0.1, 1.02) == 0, "rate limited");
    const double d = p.step(0, 1.2);
    check(std::fabs(d - 0.3) < 1e-12, "coalesced");
    check(p.step(0, 2.0) == 0, "nothing pending");
    double ap;
    const double c = dragCentre(1765.9, 0.5, 1, 1766, ap);
    check(c == 1766 && std::fabs(ap - 0.1) < 1e-9, "drag clamps to the radio limit");
    Click k = clickPlan(false, false, 100.1, 100.0, 2.0, 1, 1766);
    check(!k.retune && k.decodeOnly, "normal inside: marker only");
    k = clickPlan(false, false, 105.0, 100.0, 2.0, 1, 1766);
    check(k.retune && k.centreMhz == 105.0, "normal outside: retune");
    k = clickPlan(true, false, 100.1, 100.0, 2.0, 1, 1766);
    check(k.retune && k.centreMhz == 100.1, "centre style retunes even inside");
    k = clickPlan(true, false, 5000, 100.0, 2.0, 1, 1766);
    check(k.centreMhz == 1766, "centre style clamps");
    k = clickPlan(true, true, 100.4, 100.0, 2.0, 1, 1766);
    check(!k.retune && k.decodeOnly, "file/wide never retunes");
    DragResult r = dragWithMarker(100.0, 100.5, 0.4, 1, 1766, 2.0, false);
    check(r.centre == 100.4 && r.tuned == 100.5, "marker keeps its frequency while inside the band");
    r = dragWithMarker(100.0, 100.5, 2.0, 1, 1766, 2.0, false);
    check(r.centre == 102.0 && std::fabs(r.tuned - 101.1) < 1e-9, "marker clamps at the passband edge, no jump");
    r = dragWithMarker(100.0, 100.5, 0.4, 1, 1766, 2.0, true);
    check(std::fabs(r.tuned - 100.9) < 1e-9, "setting: tuned frequency moves with the centre");
    r = dragWithMarker(1765.9, 1766.0, 0.5, 1, 1766, 2.0, true);
    check(r.centre == 1766 && std::fabs(r.tuned - 1766.1) < 1e-9, "moving marker follows only the applied step");
    View v = viewAfterRetune(100.2, 100.4, 100.1, 2.0, true);
    check(v.lo == 100.2 && v.hi == 100.4, "zoomed view inside the new band stays (span kept)");
    v = viewAfterRetune(500, 500.5, 100, 2.0, true);
    check(std::fabs((v.hi - v.lo) - 0.5) < 1e-9 && std::fabs((v.lo + v.hi) / 2 - 100) < 1e-9, "far retune: span kept, view centred");
    v = viewAfterRetune(0, 0, 100, 2.0, false);
    check(v.lo == 99 && v.hi == 101, "no view yet: whole band");
    // the tuner retunes while a click on the spectrum left a decode shift (#262): radio on 11D (222.064 MHz), 12A (223.936) clicked = +1.872 MHz
    TunerRetune t = tunerRetune(222.064e6, 222.064e6, 1.872e6, 223.936e6);
    check(t.dialHz == 222.064e6 && !t.clearShift, "gain change: the radio and the shift stay");
    t = tunerRetune(222.064e6, 222.064e6, 1.872e6, 225.648e6);
    check(t.dialHz == 225.648e6 && t.clearShift, "a channel from the list: the radio goes there, the shift is dropped");
    t = tunerRetune(222.064e6, 222.064e6, 1.872e6, 222.064e6);
    check(t.dialHz == 222.064e6 && t.clearShift, "the radio centre typed in: no shift any more");
    t = tunerRetune(100e6, 100e6, 0, 101e6);
    check(t.dialHz == 101e6 && !t.clearShift, "no shift: the radio follows the frequency");
    // P25 (50 kHz tune offset): the radio centre is 100.05 MHz, but the receiver mixes the offset out after the shift, so 100.1 is dial + 0.1
    t = tunerRetune(100e6, 100e6, 0.1e6, 100.1e6);
    check(t.dialHz == 100e6 && !t.clearShift, "a mode tune offset: the decoded frequency counts from the radio's dial, not its centre");
        {   // zoom keeps the point under the pointer; clamps; marker snap; centring
        View v = zoomAbout(100, 102, 100.5, 0.5, 99, 103, 0.001);
        check(std::fabs(v.hi - v.lo - 1.0) < 1e-12 && std::fabs((100.5 - v.lo) / 1.0 - 0.25) < 1e-12, "zoom in keeps the anchor fraction");
        v = zoomAbout(100, 102, 100.5, 1 / 0.5, 99, 103, 0.001);
        check(std::fabs(v.hi - v.lo - 4.0) < 1e-12 && v.lo >= 99 && v.hi <= 103, "zoom out stays in the band");
        v = zoomAbout(99, 100, 99, 0.001, 99, 103, 0.001);
        check(std::fabs(v.hi - v.lo - 0.001) < 1e-12 && std::fabs(v.lo - 99) < 1e-12, "minimum span, edge anchor fixed");
        v = zoomAbout(99.5, 100.5, 100, 100.0, 99, 103, 0.001);
        check(v.lo == 99 && v.hi == 103, "full zoom out is the band");
        check(zoomAnchor(100.01, 100.0, 99, 101, 1000, 8) == 100.0, "snaps to the marker");
        check(zoomAnchor(100.2, 100.0, 99, 101, 1000, 8) == 100.2, "far from the marker: the pointer");
        check(zoomAnchor(100.0, 105.0, 99, 101, 1000, 8) == 100.0, "marker off screen: the pointer");
        v = centreView(100, 101, 100.9, 99, 103);
        check(std::fabs((v.lo + v.hi) / 2 - 100.9) < 1e-12, "view centred on the signal");
        v = centreView(100, 101, 102.9, 99, 103);
        check(v.hi == 103 && std::fabs(v.hi - v.lo - 1) < 1e-12, "view centre stops at the band edge");
        check(targetClickActs(100.0, 100.0, 99.0, true), "target click on the marker still centres");
        check(!targetClickActs(100.0, 100.0, 100.0, true), "target click on the centre does nothing");
        check(!targetClickActs(100.0, 100.0, 99.0, false), "plain click on the marker does nothing");
        bool dn = false;
        v = followMarker(99, 100, 99.1, 98, 103, 1.0, dn);
        check(dn && std::fabs((v.lo + v.hi) / 2 - 99.1) < 1e-12 && v.lo >= 98, "marker at the left lands in the centre");
        v = followMarker(99, 100, 99.1, 98, 103, 0.5, dn);
        check(!dn && v.lo < 99 && v.lo > 98.6, "an eased step moves part of the way");
        v = followMarker(98, 99, 98.1, 98, 103, 1.0, dn);
        check(v.lo == 98 && dn, "at the band edge the view stops");
    }
    return fails ? 1 : 0;
}
