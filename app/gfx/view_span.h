// The frequency view of the spectrum and the waterfall when it is zoomed by the Zoom slider or the zoom keys (plots.cpp): around the TUNED
// frequency (the VFO, the channel being listened to), kept in the middle of the view, never around the middle of the capture. Only the view
// moves: the radio's centre frequency stays. Near the edge of the captured band the view stops at the edge instead of showing empty space.
// The mouse wheel over the plots zooms around the mouse as before (plot.cpp). No ImGui here: tests/general40/test_general_layout.cpp checks it.
#pragma once
#include <algorithm>
#include <cmath>

namespace viewspan {

struct View { double lo = 0, hi = 0; };

// The view `span` wide around `anchor`, inside the captured band [bandLo, bandHi]: the span is at most the band, the anchor is in the middle
// when it can be and the view is pushed back inside the band when it is not (an anchor outside the band gives the band's nearest edge).
inline View zoomAround(double anchor, double span, double bandLo, double bandHi) {
    if (!(bandHi > bandLo)) return {bandLo, bandHi};
    const double band = bandHi - bandLo;
    if (!std::isfinite(span) || span <= 0 || span >= band) return {bandLo, bandHi};
    if (!std::isfinite(anchor)) anchor = (bandLo + bandHi) * 0.5;
    double lo = anchor - span * 0.5, hi = anchor + span * 0.5;
    if (lo < bandLo) { lo = bandLo; hi = lo + span; }
    if (hi > bandHi) { hi = bandHi; lo = hi - span; }
    return {lo, hi};
}

// A zoom step of the keys: the span times `factor` (0.8 in, 1.25 out) around the anchor
inline View zoomBy(double anchor, double lo, double hi, double factor, double bandLo, double bandHi) {
    return zoomAround(anchor, (hi - lo) * factor, bandLo, bandHi);
}

}  // namespace viewspan
