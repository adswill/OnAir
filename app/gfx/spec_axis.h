// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The frequency axis maths the spectrum and the waterfall share, as pure functions (no ImGui): where a bin and a waterfall column sit,
// the columns a zoomed view shows, how far a retune moves the history, the band-edge drag and the frame pacing. Tested in
// tests/app/test_spec_axis.cpp.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace specaxis {

// The room a plot keeps at its right edge for the last x tick label, which sticks out. The spectrum and the waterfall are drawn flush, one
// over the other, and must have the same width: there the label is dropped instead (#270: the spectrum was 12 px narrower than the waterfall).
inline float rightOverhang(bool xTickLabels, bool flush, float scale) { return xTickLabels && !flush ? 12.f * scale : 0.f; }

// The centre of bin i of an n-bin, DC-centred frame of fs around c (the trace draws its point there).
inline double binCentre(double c, double fs, size_t n, double i) { return n ? c - fs / 2 + i * fs / (double)n : c; }

// The waterfall's left edge. A column holds whole bins and a bin covers half a bin either side of its centre, so the picture starts half a
// bin left of c - fs / 2 and is fs wide: its colours then sit under the trace points they come from.
inline double imageLo(double c, double fs, size_t nBins) { return c - fs / 2 - (nBins ? fs / (2.0 * (double)nBins) : 0.0); }

// The history columns [c0, c1) under the view [lo, hi] of a picture of cols columns from imgLo over fs.
inline void visibleCols(double lo, double hi, double imgLo, double fs, int cols, int& c0, int& c1) {
    if (!(fs > 0) || cols < 1) { c0 = 0; c1 = cols; return; }
    c0 = (int)std::floor((lo - imgLo) / fs * cols);
    c1 = (int)std::ceil((hi - imgLo) / fs * cols);
    c0 = std::max(0, std::min(cols, c0)); c1 = std::max(c0, std::min(cols, c1));
}

// The columns a retune from oldC to newC moves the history by (positive: to the right, the centre went down). The rows that are there then
// line up with the new axis: a signal stays under its frequency. A jump of a whole band or more clears everything (+-cols).
inline int retuneShift(double oldC, double newC, double fs, int cols) {
    if (!(fs > 0) || cols < 1 || !std::isfinite(oldC) || !std::isfinite(newC)) return 0;
    const double s = (oldC - newC) / fs * cols;
    if (s >= cols) return cols;
    if (s <= -cols) return -cols;
    return (int)std::lround(s);
}

// One row moved by s columns (out[x] = in[x - s]); what comes in is fill.
inline void shiftRow(uint8_t* r, int cols, int s, uint8_t fill) {
    if (s == 0 || cols < 1) return;
    if (s >= cols || s <= -cols) { std::memset(r, fill, (size_t)cols); return; }
    if (s > 0) { std::memmove(r + s, r, (size_t)(cols - s)); std::memset(r, fill, (size_t)s); }
    else { std::memmove(r, r - s, (size_t)(cols + s)); std::memset(r + cols + s, fill, (size_t)-s); }
}

// Screen x and frequency on a plot from x0, w pixels wide, showing [lo, hi].
inline double pxToMhz(double px, double x0, double w, double lo, double hi) { return w > 0 ? lo + (px - x0) / w * (hi - lo) : lo; }
inline double mhzToPx(double mhz, double x0, double w, double lo, double hi) { return hi > lo ? x0 + (mhz - lo) / (hi - lo) * w : x0; }

// The band edge under the mouse: 0 the lower, 1 the upper, -1 neither (within grab pixels; the nearer one wins).
inline int edgeUnder(double mx, double pxLo, double pxHi, double grab) {
    const double dl = std::fabs(mx - pxLo), dh = std::fabs(mx - pxHi);
    if (dl > grab && dh > grab) return -1;
    return dl <= dh ? 0 : 1;
}

// The symmetric channel width while an edge is held: twice the distance from the centre to the mouse, on a step, inside [minW, maxW].
inline double widthFromEdge(double centre, double mouse, double minW, double maxW, double step) {
    double w = 2 * std::fabs(mouse - centre);
    if (step > 0) w = std::round(w / step) * step;
    return std::min(maxW, std::max(minW, w));
}

// The waterfall history width for the chosen detail (0: the standard 8192 columns): a power of two, at least 8192, at most the cap
// (65536: 32 MB of history), and no wider than the FFT gives (more columns than bins only repeat them).
// want < 0: automatic, as fine as the FFT (the default since 3.0: a zoomed-in waterfall keeps every bin).
inline int historyCols(int want, size_t fftBins, int cap = 65536) {
    int c = 8192;
    if (want < 0) want = (int)std::min<size_t>((size_t)cap, fftBins);
    if (want <= 0) return c;
    while (c < want && c < cap) c *= 2;
    while (c > 8192 && (size_t)c > fftBins) c /= 2;
    return c;
}

// The user's frequency markers (right click on the spectrum: "Add a marker here"), stored as "100.5,433.92" (MHz). At most kMaxMarkers;
// what does not read as a frequency is skipped.
constexpr int kMaxMarkers = 16;
inline std::vector<double> parseMarkers(const std::string& s) {
    std::vector<double> v;
    size_t p = 0;
    while (p < s.size() && (int)v.size() < kMaxMarkers) {
        size_t e = s.find(',', p); if (e == std::string::npos) e = s.size();
        const std::string t = s.substr(p, e - p);
        p = e + 1;
        char* end = nullptr;
        const double f = std::strtod(t.c_str(), &end);
        if (end != t.c_str() && std::isfinite(f) && f > 0) v.push_back(f);
    }
    return v;
}
inline std::string markersText(const std::vector<double>& v) {
    std::string o;
    for (size_t i = 0; i < v.size() && (int)i < kMaxMarkers; i++) { char b[32]; snprintf(b, sizeof b, "%s%.6f", i ? "," : "", v[i]); o += b; }
    return o;
}
// the marker nearest to mhz within tol (MHz), -1 for none
inline int markerNear(const std::vector<double>& v, double mhz, double tol) {
    int best = -1;
    for (size_t i = 0; i < v.size(); i++) if (std::fabs(v[i] - mhz) <= tol && (best < 0 || std::fabs(v[i] - mhz) < std::fabs(v[best] - mhz))) best = (int)i;
    return best;
}

// Frame pacing (main.cpp): the time to the next frame, 0 for unpaced. A shown window with a live waterfall keeps the spectrum's rate even
// when it is not focused, so the picture never stops moving on its own (#246); the CPU saving is for hidden windows and idle screens.
struct Pace {
    bool shot = false;           // --shot: the unpaced loop its frame counts rely on
    bool shown = true;           // visible and not minimised (or a pop-out is)
    bool reduceCpuHidden = false;
    bool recentInput = false;    // typing, dragging (the last second)
    bool video = false;          // a programme plays
    bool running = false;        // a receiver runs
    bool focused = true;
    bool liveWaterfall = false;  // a running receiver's waterfall is not paused
};
inline double frameInterval(const Pace& p) {
    if (p.shot) return 0;
    if (!p.shown) return p.reduceCpuHidden ? 0.1 : 0.25;
    if (p.recentInput || p.video) return 1.0 / 60;
    if (p.running) return p.focused || p.liveWaterfall ? 1.0 / 30 : 1.0 / 15;
    return p.focused ? 0.1 : 0.25;
}

// Spectrum frames the screen did not take (it drew slower than they came): the waterfall repeats the newest row for them, at most cap,
// so its time scale stays true. seq counts the frames; 0 when this is the first one or nothing was missed.
inline int missedRows(uint64_t lastSeq, uint64_t seq, int cap = 8) {
    if (lastSeq == 0 || seq <= lastSeq + 1) return 0;
    return (int)std::min<uint64_t>((uint64_t)cap, seq - lastSeq - 1);
}

} // namespace specaxis
