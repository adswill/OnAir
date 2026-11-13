// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pure decisions for dragging the spectrum and for the two click-tuning styles (tests/app/test_tune_drag.cpp).
#pragma once
#include <algorithm>
#include <cmath>
namespace tunedrag {
// a press is a click (tunes) while it moves under 4 px; a slow still press is still a click; a move of 4 px or more is a drag (pans)
inline bool isDrag(double dxPx, double dyPx, double heldSeconds) {
    (void)heldSeconds;
    return std::fabs(dxPx) >= 4 || std::fabs(dyPx) >= 4;
}
// the radio centre may not leave the radio's range
inline double clampCentre(double centreMhz, double lo, double hi) { return std::min(hi, std::max(lo, centreMhz)); }
// A drag that ran past the captured band by overflowMhz: add it to pending, and hand out the retune step at most every minInterval seconds
// (coalesced: one step carries everything that piled up, never a queue). Returns the delta to retune by, 0 for none yet.
struct Pacer {
    double pending = 0, last = -1e9;
    double step(double overflowMhz, double now, double minInterval = 0.07) {
        pending += overflowMhz;
        if (pending == 0 || now - last < minInterval) return 0;
        const double d = pending; pending = 0; last = now; return d;
    }
};
// the new radio centre for a drag step: clamped to the radio's range; the step actually applied comes back through applied
inline double dragCentre(double centreMhz, double delta, double lo, double hi, double& applied) {
    const double c = clampCentre(centreMhz + delta, lo, hi); applied = c - centreMhz; return c;
}
// A drag step with the tuned marker: the radio centre moves (clamped), the tuned (demodulated) frequency stays where it is, and is only
// clamped at the edge of the new passband (edgeFrac of fs around the centre) when the centre would carry it out of the band.
// movesTuned (the setting): the tuned frequency travels with the centre instead.
struct DragResult { double centre, tuned; };
inline DragResult dragWithMarker(double centre, double tuned, double delta, double lo, double hi, double fsMhz, bool movesTuned) {
    const double c = clampCentre(centre + delta, lo, hi);
    if (movesTuned) return {c, tuned + (c - centre)};
    const double e = fsMhz * 0.45;
    return {c, std::min(c + e, std::max(c - e, tuned))};
}
// The view [lo,hi] after the radio centre went from oldC to newC: the span is kept (at most fs). A view still inside the new band stays where
// it is (a drag); otherwise it is centred on the new centre (a click that tuned far away).
struct View { double lo, hi; };
inline View viewAfterRetune(double lo, double hi, double newC, double fsMhz, bool valid) {
    const double bLo = newC - fsMhz / 2, bHi = newC + fsMhz / 2;
    if (!valid || !(hi > lo)) return {bLo, bHi};
    const double span = std::min(fsMhz, hi - lo);
    if (lo >= bLo - 1e-9 && hi <= bHi + 1e-9) return {lo, hi};
    if (lo < bHi && hi > bLo && span < fsMhz) {   // partly overlapping: slide it into the band (a drag that ran to the edge)
        const double l = std::min(bHi - span, std::max(bLo, lo));
        return {l, l + span};
    }
    return {newC - span / 2, newC + span / 2};
}
// A retune from the tuner (frequency box, channel list, gain, filter, bias-tee) while a decode shift (sticky tuning) may be active: the receiver
// decodes runDialHz + shiftHz (the running radio's dial: its centre less the mode's tune offset, which the receiver mixes out after the shift).
// When that is still the tuned frequency (a gain or filter change) the radio stays where it is (dialHz) and so does the shift. A new frequency
// puts the radio on it and drops the shift: kept, the receiver would decode beside the new channel by the old shift (#262: a DAB channel picked
// from the list after a click on the spectrum).
struct TunerRetune { double dialHz; bool clearShift; };
inline TunerRetune tunerRetune(double dialHz, double runDialHz, double shiftHz, double tunedHz) {
    if (shiftHz != 0 && std::fabs(runDialHz + shiftHz - tunedHz) <= 1) return {dialHz, false};
    return {tunedHz, shiftHz != 0};
}

// ---- zoom, pan and the target (centre) click: pure maths, tested in tests/app/test_tune_drag.cpp ----
// Zoom the view [lo,hi] by factor (<1 zooms in) so that `anchor` keeps its place on screen (its fraction of the width), like SDR++. The span stays in
// [minSpan, band width] and the view inside [bLo,bHi]; the anchor only moves when the band edge forces it.
inline View zoomAbout(double lo, double hi, double anchor, double factor, double bLo, double bHi, double minSpan) {
    const double bw = bHi - bLo;
    if (!(hi > lo) || !(bw > 0)) return {bLo, bHi};
    const double r = hi - lo, m = std::min(1.0, std::max(0.0, (anchor - lo) / r));
    const double ns = std::min(bw, std::max(std::min(minSpan, bw), r * factor));
    double n0 = anchor - m * ns;
    n0 = std::max(bLo, std::min(n0, bHi - ns));
    return {n0, n0 + ns};
}
// The point a wheel zoom keeps still: the tuner marker when the pointer is within snapPx of it, else the pointer. The marker has to be on screen.
inline double zoomAnchor(double pointerMhz, double markerMhz, double lo, double hi, double widthPx, double snapPx) {
    if (!(hi > lo) || !(widthPx > 0) || markerMhz < lo || markerMhz > hi) return pointerMhz;
    return std::fabs(pointerMhz - markerMhz) / (hi - lo) * widthPx <= snapPx ? markerMhz : pointerMhz;
}
// The same span centred on c, inside the band (the view after a target click: the signal sits in the middle of the screen)
inline View centreView(double lo, double hi, double c, double bLo, double bHi) {
    const double s = std::min(bHi - bLo, hi - lo);
    const double n0 = std::max(bLo, std::min(c - s / 2, bHi - s));
    return {n0, n0 + s};
}
// One easing step of the view towards having the marker in the middle (same span, inside the band). k in (0,1]: 1 jumps. done: the marker is centred.
inline View followMarker(double lo, double hi, double marker, double bLo, double bHi, double k, bool& done) {
    const View t = centreView(lo, hi, marker, bLo, bHi);
    const double span = hi - lo;
    View v{lo + (t.lo - lo) * k, hi + (t.hi - hi) * k};
    done = std::fabs(t.lo - v.lo) + std::fabs(t.hi - v.hi) < span * 1e-4 || k >= 1;
    if (done) v = t;
    return v;
}
// A click in target style must act unless it is already on the radio centre: comparing with the tuned frequency (marker) ignored a click on the
// marker, and the grid snap made a click near it the same number, so a signal tuned off-centre could never be centred by clicking it.
inline bool targetClickActs(double clickedMhz, double tunedMhz, double radioCentreMhz, bool centreStyle) {
    if (centreStyle) return std::fabs(clickedMhz - radioCentreMhz) > 1e-6;
    return std::fabs(clickedMhz - tunedMhz) > 1e-6;
}
struct Click { bool retune; double centreMhz; bool decodeOnly; };
// What a click on absMhz does. centreStyle: the target button (centre the radio on the click); else the click only moves the marker
// inside the captured band and retunes only when it lies outside it. noRetune: a file or a wide mode: the radio never moves.
inline Click clickPlan(bool centreStyle, bool noRetune, double absMhz, double centreMhz, double fsMhz, double lo, double hi) {
    if (noRetune) return {false, centreMhz, true};
    const bool inside = std::fabs(absMhz - centreMhz) < fsMhz * 0.45;
    if (!centreStyle && inside) return {false, centreMhz, true};
    return {true, clampCentre(absMhz, lo, hi), false};
}
}
