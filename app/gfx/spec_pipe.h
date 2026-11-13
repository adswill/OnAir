// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The spectrum-to-picture pipeline's pure steps, shared by the app and the offscreen render harness (tools/spec_render.cpp):
//  - the noise floor of a spectrum (the median of its lowest fifth of bins) and a tracker that follows it with a time constant,
//  - the waterfall's auto range anchored on that floor (the floor sits at a fixed place of the colour map, the range a fixed width),
//  - a spectrum row onto the history columns (the highest bin of each column) as levels 0..255.
// No ImGui, no App. Tested in tests/app/test_spec_look.cpp.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace spipe {

// The noise floor of n bins: the median of the lowest 20 % (about the 10th percentile), from at most ~2048 bins evenly picked (cheap at
// 256k bins). A carrier or a TV block covering most of the span still leaves its gaps as the floor. scratch is reused (no allocation).
inline float noiseFloor(const float* s, size_t n, std::vector<float>& scratch) {
    if (!s || n == 0) return NAN;
    const size_t step = std::max<size_t>(1, n / 2048);
    scratch.clear();
    for (size_t i = 0; i < n; i += step) if (std::isfinite(s[i])) scratch.push_back(s[i]);
    if (scratch.empty()) return NAN;
    const size_t k = scratch.size() / 10;
    std::nth_element(scratch.begin(), scratch.begin() + (long)k, scratch.end());
    return scratch[k];
}

// Follows the floor with a time constant (seconds): the colours never jump with one frame, and a retune settles in about tau.
// A jump of more than `snap` dB (a gain change, a new band) is taken at once.
struct FloorTracker {
    float floor = NAN;
    float tau = 2.f, snap = 15.f;
    float update(float f, float dt) {
        if (!std::isfinite(f)) return floor;
        if (!std::isfinite(floor) || std::fabs(f - floor) > snap) floor = f;
        else floor += (f - floor) * (1.f - std::exp(-std::max(0.f, dt) / std::max(0.01f, tau)));
        return floor;
    }
};

// The auto range: the colour map starts `below` dB under the floor and spans `span` dB, so the floor always lands at below / span of
// the map (its dark end, with the grain of the noise visible) and a signal `span - below` dB over the floor reaches the top.
inline void floorRange(float floorDb, float below, float span, float& lo, float& hi) {
    span = std::max(10.f, span);
    lo = floorDb - below;
    hi = lo + span;
}

// One spectrum row of n bins onto C history columns as levels 0..255 over [minDb, maxDb]: each column the highest bin it covers (no
// carrier lost between columns). The usual case (n a multiple of C) runs a fixed-length max the compiler vectorises.
inline void levelsRow(const float* d, size_t n, int C, float minDb, float maxDb, uint8_t* l) {
    const float inv = 255.f / std::max(1.f, maxDb - minDb);
    if (n >= (size_t)C && n % (size_t)C == 0) {
        const size_t k = n / (size_t)C;
        for (int x = 0; x < C; x++) {
            const float* c = d + (size_t)x * k;
            float m = c[0];
            for (size_t j = 1; j < k; j++) m = std::max(m, c[j]);
            const float v = (m - minDb) * inv;
            l[x] = (uint8_t)(v <= 0.f ? 0 : v >= 255.f ? 255 : (int)(v + 0.5f));
        }
        return;
    }
    for (int x = 0; x < C; x++) {
        const size_t a = (size_t)x * n / (size_t)C, b = std::max(a + 1, (size_t)(x + 1) * n / (size_t)C);
        float m = d[std::min(a, n - 1)];
        for (size_t k = a + 1; k < b && k < n; k++) m = std::max(m, d[k]);
        const float v = (m - minDb) * inv;
        l[x] = (uint8_t)(v <= 0.f ? 0 : v >= 255.f ? 255 : (int)(v + 0.5f));
    }
}

// The pixel-exact mapping of one row of levels (4.0, docs/WATERFALL-4.0.md): cols columns side by side over [rowLo, rowHi) onto px pixel
// columns showing [visLo, visHi). Where columns are narrower than pixels, each pixel is the highest of the columns whose centre falls in it
// (every column lands in exactly one pixel: a one-bin signal is never lost and never doubled); where they are wider, each pixel takes the
// column under its centre (sharp steps: no level is made up between two columns). Pixels outside the row get fill. Returns the pixels set.
inline int pixelLevels(const uint8_t* row, int cols, double rowLo, double rowHi, double visLo, double visHi, int px, uint8_t* out, uint8_t fill) {
    if (px < 1) return 0;
    if (!row || cols < 1 || !(rowHi > rowLo) || !(visHi > visLo)) { std::memset(out, fill, (size_t)px); return 0; }
    const double cw = (rowHi - rowLo) / cols, pw = (visHi - visLo) / px;
    int set = 0;
    if (cw >= pw) {
        for (int x = 0; x < px; x++) {
            const double c = std::floor((visLo + (x + 0.5) * pw - rowLo) / cw);
            if (c < 0 || c >= cols) { out[x] = fill; continue; }
            out[x] = row[(int)c]; set++;
        }
        return set;
    }
    for (int x = 0; x < px; x++) {   // the columns c with (c + 0.5) cw in [a, b)
        const double a = (visLo + x * pw - rowLo) / cw - 0.5, b = (visLo + (x + 1) * pw - rowLo) / cw - 0.5;
        long c0 = (long)std::ceil(a - 1e-9), c1 = (long)std::ceil(b - 1e-9) - 1;
        c0 = std::max(0L, c0); c1 = std::min((long)cols - 1, c1);
        if (c1 < c0) { out[x] = fill; continue; }
        uint8_t m = row[c0];
        for (long c = c0 + 1; c <= c1; c++) m = std::max(m, row[c]);
        out[x] = m; set++;
    }
    return set;
}

// The same for dB values (the trace's peak-keeping reduction on real bins): bins centred at x0 + i * dx. NAN where no bin.
inline void pixelPeaks(const float* y, size_t n, double x0, double dx, double visLo, double visHi, int px, float* out) {
    if (px < 1) return;
    const double pw = (visHi - visLo) / px;
    for (int x = 0; x < px; x++) {
        if (!y || n == 0 || !(dx > 0) || !(pw > 0)) { out[x] = NAN; continue; }
        long c0, c1;
        if (dx >= pw) {
            const double c = std::floor((visLo + (x + 0.5) * pw - (x0 - dx * 0.5)) / dx);
            out[x] = c < 0 || c >= (double)n ? NAN : y[(size_t)c];
            continue;
        } else {
            c0 = (long)std::ceil((visLo + x * pw - x0) / dx - 1e-9);
            c1 = (long)std::ceil((visLo + (x + 1) * pw - x0) / dx - 1e-9) - 1;
        }
        c0 = std::max(0L, c0); c1 = std::min((long)n - 1, c1);
        if (c1 < c0) { out[x] = NAN; continue; }
        float m = y[c0];
        for (long c = c0 + 1; c <= c1; c++) m = std::max(m, y[c]);
        out[x] = m;
    }
}

// The averaged trace: an exponential average with time constant tau seconds (0: the raw frame), so the look does not depend on the frame rate
inline void emaStep(float* sm, const float* in, size_t n, float dt, float tau) {
    const float alpha = tau <= 0.001f ? 1.f : 1.f - std::exp(-dt / tau);
    for (size_t i = 0; i < n; i++) sm[i] += alpha * (in[i] - sm[i]);
}

// 10^(db/10) and 10*log10(p) as branch-free float maths the compiler vectorises (about 0.01 dB): the power-domain average runs over every
// bin of every frame (64k bins, 30 frames a second), where the library calls cost more than the FFT
inline float fastPow10db(float db) {
    float x = db * 0.33219281f;                      // log2(10) / 10
    x = std::fmax(-126.f, std::fmin(126.f, x));
    const float fl = std::floor(x), f = x - fl;
    const float p = 1.f + f * (0.69314718f + f * (0.24022650f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
    int32_t e = ((int32_t)fl + 127) << 23; float s; std::memcpy(&s, &e, 4);
    return s * p;
}
inline float fastDb10(float p) {
    p = std::fmax(p, 1e-30f);
    int32_t u; std::memcpy(&u, &p, 4);
    const float e = (float)(((u >> 23) & 0xFF) - 127);
    u = (u & 0x007FFFFF) | 0x3F800000; float m; std::memcpy(&m, &u, 4);   // m in [1, 2)
    const float z = (m - 1.f) / (m + 1.f), z2 = z * z;
    const float l2 = 2.88539008f * z * (1.f + z2 * (1.f / 3 + z2 * (1.f / 5 + z2 * (1.f / 7))));
    return 3.01029996f * (e + l2);
}

// The trace's time constant grows with the bins each pixel column covers: the column shows the highest of them (no carrier is lost), and
// the highest of many noisy bins is a rough comb unless each bin is averaged longer. sqrt: 4x the bins, 2x the averaging; 1.6x to 8x.
inline float traceTau(float baseTau, double binsPerPixel) {
    if (baseTau <= 0.001f) return baseTau;
    return baseTau * (float)std::min(8.0, std::max(1.6, 1.5 * std::sqrt(std::max(1.0, binsPerPixel) / 2.0)));
}

// The average in the POWER domain (a dB average reads noise ~2.5 dB low and lets the deep nulls of single bins through): pw keeps the
// average power per bin, out gets it in dB. alpha = 1 - exp(-dt / tau).
inline void powerEma(std::vector<float>& pw, const float* inDb, float* outDb, size_t n, float alpha) {
    if (pw.size() != n) { pw.resize(n); for (size_t i = 0; i < n; i++) pw[i] = fastPow10db(inDb[i]); alpha = 1.f; }
    float* p = pw.data();
    for (size_t i = 0; i < n; i++) { p[i] += alpha * (fastPow10db(inDb[i]) - p[i]); outDb[i] = fastDb10(p[i]); }
}
inline float alphaFor(float dt, float tau) { return tau <= 0.001f ? 1.f : 1.f - std::exp(-dt / tau); }

// dB per bin to dB per Hz: what a bin of fs / n Hz holds less 10*log10(fs / n), so the noise floor stays where it is when the FFT size changes
inline float perHzOffset(double fsHz, size_t n) { return fsHz > 0 && n > 0 ? (float)(10.0 * std::log10(fsHz / (double)n)) : 0.f; }

// Roughness of a trace: the mean absolute difference between neighbouring values (dB); the test keeps the noise floor's under 1.5 dB
inline double roughness(const float* v, size_t n) {
    if (n < 2) return 0;
    double s = 0;
    for (size_t i = 1; i < n; i++) s += std::fabs((double)v[i] - v[i - 1]);
    return s / (double)(n - 1);
}

} // namespace spipe
