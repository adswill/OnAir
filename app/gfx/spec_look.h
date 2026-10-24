// The spectrum and waterfall look and the pure maths behind it, no ImGui: OnAir's one look (measured against reference screenshots,
// docs/SPECTRUM-RESEARCH.md), the noise floor's place in each palette, the frequency ticks (1-2-5 steps, minor ticks, labels that never
// touch or leave the panel, kHz / MHz, or offsets from the tuned frequency), the trace at one point per pixel column (the highest bin of
// the column, so no peak is lost; between bins linear or cubic), its mild smoothing, the level interpolation and the 2D low-pass of the
// waterfall view, and a synthetic test spectrum (FM carriers, an OFDM block, an SSB voice) for the screenshots.
// Tested in tests/app/test_spec_look.cpp.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace slook {

struct Rgba { float r = 0, g = 0, b = 0, a = 1; };

enum Look { OnAir, NumLooks };
enum LabelMode { LabelsAbsolute, LabelsOffset };
enum Interp { InterpLinear, InterpCubic };

// What a look sets. OnAir keeps the plots exactly as they were (classic = true: the theme's colours, the min/max envelope trace, the
// labels inside the plot); the others draw everything from here.
struct LookSet {
    const char* name;
    bool classic;
    Rgba bgTop, bgBottom;        // the spectrum background, a vertical gradient
    Rgba grid;                   // grid lines (vertical, and horizontal when gridH)
    bool gridH;
    float wfGridAlpha;           // the vertical grid lines carried down over the waterfall (0: none)
    float wfGridHAlpha;          // and the waterfall's time lines
    int tracePreset;             // cmap::kTracePresets index the look picks
    float traceWeight;
    bool fill;                   // the area under the trace
    float fillTop, fillBottom;   // its alpha at the trace and at the bottom (a vertical gradient)
    int smoothPasses;            // [1 2 1] passes over the pixel trace (mild smoothing)
    int interp;                  // between bins when zoomed in: Interp
    Rgba bandTop, bandBottom;    // the passband on the spectrum (a vertical gradient)
    Rgba bandEdge;  float bandEdgeW;
    Rgba centre;    float centreW;
    Rgba wfBand;                 // the passband over the waterfall (alpha 0: none)
    float wfLineAlpha;           // the centre and edge lines over the waterfall, times their colours' alpha
    bool strip;                  // a dark axis strip under the spectrum with big labels
    Rgba stripBg, label;
    float labelScale;            // label font size, times the interface font
    bool monoLabels;
    int labelMode;               // LabelMode
    int wfMap;                   // cmap::Map the look picks (-1: keep)
    bool wfSmooth;               // the waterfall view interpolates levels between columns (no stair steps)
    bool tooltipBox;             // the cursor readout as a floating box
    Rgba tipBg, tipBorder, tipText;
    float wfBelow = 4.f, wfSpan = 55.f;   // the waterfall auto range: the map starts wfBelow dB under the noise floor and spans wfSpan dB
    float specBelow = 10.f;               // the spectrum's auto dB range: the noise floor this far over the bottom edge
};

// the colour maps (colormap.h) by number (they are at the end of cmap::Map)
constexpr int kMapSdrppSmooth = 13, kMapOxide = 14, kMapNavyHeat = 15, kMapBlueGreen = 16;
// the trace presets (colormap.h): cyan, light blue, pale cyan, pale steel
constexpr int kTraceCyan = 3, kTraceLightBlue = 8, kTracePaleCyan = 9, kTracePaleSteel = 10;

// OnAir's look (one look; the palette, the trace colour and the rest stay the user's): black, a (35,35,35) grid both ways, a pale cyan
// trace (140,226,237) over a fill of the same colour at alpha 0.2, the tuned channel a white block at alpha 0.2 with a red edge and a yellow
// centre, absolute frequency labels on a black strip, the Navy heat waterfall with the noise floor at its dark end. Any saved look number (the old
// presets) reads as this one.
inline const LookSet& lookSet(int) {
    static const LookSet k = {"OnAir", false, {0, 0, 0, 1}, {0, 0, 0, 1}, {0.78f, 0.80f, 0.82f, 0.17f}, true, 0.f, 0.f, kTracePaleCyan, 1.5f, true, 0.21f, 0.21f, 0,
         InterpCubic, {1, 1, 1, 0.20f}, {1, 1, 1, 0.20f}, {1.f, 0.10f, 0.08f, 0.95f}, 1.f, {1.f, 0.95f, 0.10f, 0.95f}, 1.f, {1, 1, 1, 0.f}, 0.f,
         true, {0, 0, 0, 1}, {0.96f, 0.96f, 0.96f, 1}, 1.35f, false, LabelsAbsolute, kMapNavyHeat, true, true,
         {0.06f, 0.06f, 0.07f, 0.92f}, {0.5f, 0.5f, 0.52f, 0.8f}, {0.95f, 0.95f, 0.95f, 1}, 3.f, 80.f, 3.f};
    return k;
}

// Where a palette puts the noise floor (the waterfall's auto range): the map starts `below` dB under the floor and spans `span` dB.
// Measured: Navy heat on spectrum-ref-44 (the floor in the map's dark end, (0,0,41)), Blue green on spectrum-ref-43 (the floor in its
// mid blue, (1,5,124)); the others start a little under the floor.
inline void floorPlace(int map, float& below, float& span) {
    if (map == kMapNavyHeat) { below = 3.f; span = 80.f; }
    else if (map == kMapBlueGreen) { below = 8.f; span = 48.f; }
    else { below = 4.f; span = 60.f; }
}

// ---------------------------------------------------------------- frequency ticks

// 1, 2 or 5 times a power of ten, the smallest such step not below raw
inline double step125(double raw) {
    if (!(raw > 0) || !std::isfinite(raw)) return 1;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double n = raw / mag;
    return (n <= 1 + 1e-9 ? 1 : n <= 2 + 1e-9 ? 2 : n <= 5 + 1e-9 ? 5 : 10) * mag;
}
inline double nextStep125(double s) { return step125(s * 1.0001); }

struct Tick { double mhz; bool major; std::string label; };

// the unit the labels of an axis over [lo, hi] MHz use: 0 kHz (below 1 MHz), 1 MHz, 2 GHz (10 GHz and up)
inline int freqUnit(double lo, double hi) {
    const double m = std::max(std::fabs(lo), std::fabs(hi));
    return m < 1.0 ? 0 : m >= 10000.0 ? 2 : 1;
}
inline const char* unitName(int u) { return u == 0 ? "kHz" : u == 2 ? "GHz" : "MHz"; }

// the decimals a step needs to be written exactly (0.25: 2, 2.5: 1, 5: 0), at most 7
inline int decimalsFor(double step) {
    step = std::fabs(step);
    for (int d = 0; d < 7; d++) { const double v = step * std::pow(10.0, d); if (std::fabs(v - std::round(v)) < 1e-6 * std::max(1.0, v)) return d; }
    return 7;
}

// a tick label: absolute in the axis unit with just the decimals the step needs; or the offset from ref with K / M ("-20K", "0", "+1.5M")
inline std::string tickLabel(double mhz, double stepMhz, int mode, double ref, int unit) {
    char b[48];
    if (mode == LabelsOffset) {
        const double d = mhz - ref;
        if (std::fabs(d) < stepMhz * 1e-6) return "0";
        const bool meg = stepMhz >= 1.0 - 1e-9;
        const double v = meg ? d : d * 1000.0, st = meg ? stepMhz : stepMhz * 1000.0;
        snprintf(b, sizeof b, "%.*f%s", std::min(4, decimalsFor(st)), v, meg ? "M" : "K");
        return b;
    }
    const double f = unit == 0 ? 1000.0 : unit == 2 ? 0.001 : 1.0;
    const double v = mhz * f, st = stepMhz * f;
    snprintf(b, sizeof b, "%.*f", decimalsFor(st), std::fabs(v) < st * 1e-6 ? 0.0 : v);
    return b;
}

// The ticks of a frequency axis [lo, hi] over widthPx pixels. The major step is the smallest 1-2-5 step whose labels (charPx per
// character, at least gapPx apart) never touch; minor ticks divide it in 5 (or 4 for a 2 step) when they are at least minorPx apart. A
// label that would leave the axis is left off (its tick stays). Labels as tickLabel(); ref is the tuned frequency for offsets.
inline std::vector<Tick> freqTicks(double lo, double hi, double widthPx, double charPx, double gapPx, int mode, double ref, double minorPx = 6) {
    std::vector<Tick> out;
    if (!(hi > lo) || !(widthPx > 1) || !std::isfinite(lo) || !std::isfinite(hi)) return out;
    const int unit = freqUnit(lo, hi);
    const double perPx = (hi - lo) / widthPx;
    double step = step125(perPx * std::max(8.0, gapPx + 3 * charPx));
    for (int guard = 0; guard < 40; guard++) {   // grow the step until the longest label fits in it
        size_t longest = 0;
        for (double v = std::ceil(lo / step - 1e-9) * step; v <= hi + step * 1e-9; v += step) longest = std::max(longest, tickLabel(v, step, mode, ref, unit).size());
        if ((double)longest * charPx + gapPx <= step / perPx) break;
        step = nextStep125(step);
    }
    const double lead = step / std::pow(10.0, std::floor(std::log10(step) + 1e-9));
    const int div = std::fabs(lead - 2) < 1e-6 ? 4 : 5;
    const double minor = step / div;
    const bool minors = minor / perPx >= minorPx;
    const double first = std::ceil(lo / (minors ? minor : step) - 1e-9) * (minors ? minor : step);
    const double inc = minors ? minor : step;
    for (int i = 0; first + i * inc <= hi + inc * 1e-9 && out.size() < 2000; i++) {
        const double v = first + i * inc;
        const double q = v / step;
        const bool major = std::fabs(q - std::round(q)) < 1e-6;
        Tick t{major ? std::round(q) * step : v, major, std::string()};
        if (major) {
            t.label = tickLabel(t.mhz, step, mode, ref, unit);
            const double x = (t.mhz - lo) / perPx, half = t.label.size() * charPx * 0.5;
            if (x - half < 0 || x + half > widthPx) t.label.clear();   // would leave the axis
        }
        out.push_back(t);
    }
    return out;
}

// ---------------------------------------------------------------- the trace at the width of the plot

// One value per pixel column of [lo, hi] over px columns, from bins at x0 + i * dx. Where bins are denser than pixels, the highest bin
// whose centre falls in the column (useMin: the lowest), so a one-bin peak always shows; where they are sparser, interpolated at the column
// centre (linear, or Catmull-Rom cubic for a smooth curve). out gets px values; false when there is nothing to show.
inline bool decimate(const float* y, size_t n, double x0, double dx, double lo, double hi, int px, int interp, bool useMin, std::vector<float>& out) {
    out.clear();
    if (!y || n < 2 || px < 1 || !(hi > lo) || !(dx > 0)) return false;
    out.resize((size_t)px);
    const double cw = (hi - lo) / px;
    auto at = [&](double fi) -> float {   // the trace at fractional bin fi
        if (fi <= 0) return y[0];
        if (fi >= (double)(n - 1)) return y[n - 1];
        const size_t i = (size_t)fi;
        const float f = (float)(fi - (double)i);
        if (interp == InterpCubic) {
            const float p0 = y[i > 0 ? i - 1 : 0], p1 = y[i], p2 = y[i + 1], p3 = y[i + 2 < n ? i + 2 : n - 1];
            const float v = p1 + 0.5f * f * (p2 - p0 + f * (2 * p0 - 5 * p1 + 4 * p2 - p3 + f * (3 * (p1 - p2) + p3 - p0)));
            const float a = std::min(p1, p2), b = std::max(p1, p2);
            return std::min(b + (b - a) * 0.25f, std::max(a - (b - a) * 0.25f, v));   // a little overshoot only: no rings
        }
        return y[i] + (y[i + 1] - y[i]) * f;
    };
    for (int c = 0; c < px; c++) {
        const double a = lo + c * cw, b = a + cw;
        // bins whose centre is in [a, b)
        long i0 = (long)std::ceil((a - x0) / dx - 1e-9), i1 = (long)std::ceil((b - x0) / dx - 1e-9) - 1;
        i0 = std::max(0L, i0); i1 = std::min((long)n - 1, i1);
        if (i1 - i0 >= 1 || (i1 == i0 && cw >= dx)) {
            float m = y[i0];
            for (long i = i0 + 1; i <= i1; i++) m = useMin ? std::min(m, y[i]) : std::max(m, y[i]);
            out[(size_t)c] = m;
        } else out[(size_t)c] = at(((a + b) * 0.5 - x0) / dx);
    }
    return true;
}

// [1 2 1] / 4 passes over the pixel trace (mild smoothing), the ends kept
inline void smoothPasses(std::vector<float>& v, int passes) {
    const size_t n = v.size();
    if (n < 3) return;
    std::vector<float> t(n);
    for (int p = 0; p < passes; p++) {
        t[0] = v[0]; t[n - 1] = v[n - 1];
        for (size_t i = 1; i + 1 < n; i++) t[i] = 0.25f * v[i - 1] + 0.5f * v[i] + 0.25f * v[i + 1];
        v.swap(t);
    }
}

// ---------------------------------------------------------------- levels and range

// the p-th fraction (0..1) of v (a copy is sorted partly), NAN for none
inline float percentile(std::vector<float> v, double p) {
    if (v.empty()) return NAN;
    const size_t k = (size_t)std::min((double)v.size() - 1, std::max(0.0, p * (double)(v.size() - 1) + 0.5));
    std::nth_element(v.begin(), v.begin() + (long)k, v.end());
    return v[k];
}

// The colour range from the noise floor (a low percentile of the spectrum) and the strongest signal: lo sits below dB under the floor,
// hi headroom dB over the top but at most maxAbove over the floor, and the range is at least minSpan.
inline void autoRange(float floorDb, float topDb, float below, float headroom, float maxAbove, float minSpan, float& lo, float& hi) {
    lo = std::floor(floorDb - below);
    hi = std::ceil(std::min(topDb + headroom, floorDb + maxAbove));
    if (hi < lo + minSpan) hi = lo + minSpan;
}

// a level 0..255 between two columns (f 0..1 of the way): the colour then comes from the map, not from blending two colours
inline uint8_t lerpLevel(uint8_t a, uint8_t b, float f) { return (uint8_t)std::lround(a + (b - a) * std::min(1.f, std::max(0.f, f))); }

// The waterfall view's low-pass: one row from itself and its newer and older neighbours (null: not there, the row itself) [1 2 1] / 4,
// then [1 2 1] / 4 across. out gets n levels.
inline void lowpassRow(const uint8_t* newer, const uint8_t* cur, const uint8_t* older, int n, uint8_t* out) {
    if (n < 1) return;
    std::vector<int> v((size_t)n);
    for (int i = 0; i < n; i++) v[(size_t)i] = (newer ? newer[i] : cur[i]) + 2 * cur[i] + (older ? older[i] : cur[i]);   // x4
    if (n == 1) { out[0] = (uint8_t)((v[0] + 2) / 4); return; }
    for (int i = 0; i < n; i++) {
        const int l = v[(size_t)(i > 0 ? i - 1 : i)], r = v[(size_t)(i + 1 < n ? i + 1 : i)];
        out[i] = (uint8_t)std::min(255, (l + 2 * v[(size_t)i] + r + 8) / 16);
    }
}

// ---------------------------------------------------------------- a test spectrum (screenshots, --specdemo)

// n bins of dBFS over fs MHz around centre c at time t (seconds): a noise floor near -100 dB with a gentle slope, three FM carriers
// (one with stereo pilot and RDS humps), a flat 7.6 MHz-wide OFDM block (with pilots), a 2.7 kHz SSB voice that comes and goes,
// and a couple of narrow carriers. seed: the noise; the same t and seed give the same frame.
inline void demoSpectrum(std::vector<float>& out, size_t n, double c, double fs, double t, uint32_t seed = 1) {
    out.assign(n, -100.f);
    uint32_t s = seed * 2654435761u + (uint32_t)(t * 1000.0) * 40503u + 1;
    auto rnd = [&] { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (s & 0xFFFFFF) / 16777216.0f; };
    const double lo = c - fs / 2, dx = fs / (double)n;
    auto pw = [](float db) { return std::pow(10.f, db / 10.f); };
    std::vector<float> p(n, 0.f);
    for (size_t i = 0; i < n; i++) {
        const double f = lo + (i + 0.5) * dx, u = (f - c) / fs;
        float floor = -101.f + 2.f * (float)std::cos(u * 3.1);
        // exponential (chi-square 2) noise, as an FFT bin of noise: power floor * -ln(U)
        const float e = -std::log(std::max(1e-6f, rnd()));
        p[i] = pw(floor) * e;
    }
    auto add = [&](double fMhz, double widthMhz, float peakDb, int shape) {   // shape 0 gaussian, 1 flat with steep sides, 2 voice
        const double sigma = std::max(widthMhz, dx * 0.6);
        const long ic = (long)((fMhz - lo) / dx), half = (long)(4 * sigma / dx) + 2;
        for (long i = std::max(0L, ic - half); i <= std::min((long)n - 1, ic + half); i++) {
            const double f = lo + (i + 0.5) * dx, d = (f - fMhz) / sigma;
            float g;
            if (shape == 1) g = (float)(1.0 / (1.0 + std::pow(std::fabs(d) * 2.0, 24.0)));
            else if (shape == 2) { const double x = (f - fMhz) / widthMhz; g = x < 0.11 || x > 1 ? 0.f : (float)(std::exp(-3 * x) * (0.6 + 0.4 * std::sin(x * 40 + t * 3))); }
            else g = (float)std::exp(-0.5 * d * d);
            if (g > 1e-5f) p[(size_t)i] += pw(peakDb) * g * (shape == 1 ? (0.75f + 0.5f * rnd()) : (0.8f + 0.4f * rnd()));
        }
    };
    // FM: three stations, each a 100 kHz-wide hump with the carrier on top
    const double fm[3] = {c - fs * 0.36, c - fs * 0.30, c + fs * 0.34};
    for (int k = 0; k < 3; k++) {
        add(fm[k], fs * 0.0045, -52.f - 9.f * k, 0);
        add(fm[k], fs * 0.0012, -46.f - 9.f * k, 0);
    }
    // OFDM: 7.6 MHz-like block, a fifth of the band wide, left of centre
    add(c - fs * 0.06, fs * 0.19, -72.f, 1);
    for (int k = -8; k <= 8; k++) add(c - fs * 0.06 + k * fs * 0.0105, dx * 0.7, -67.f, 0);   // the continual pilots
    // SSB voice: upper sideband, 2.7 kHz-like, fading in and out
    const float on = (float)(0.5 + 0.5 * std::sin(t * 1.3));
    add(c + fs * 0.12, fs * 0.012, -70.f + 14.f * on, 2);
    // narrow carriers
    add(c + fs * 0.215, dx, -62.f, 0);
    add(c + fs * 0.26, dx, -80.f, 0);
    for (size_t i = 0; i < n; i++) out[i] = 10.f * std::log10(std::max(1e-30f, p[i]));
}

} // namespace slook
