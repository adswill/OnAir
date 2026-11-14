// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Weather fax hand phasing: a circular sideways shift of every line (the picture as received, before any rotation), for a picture whose phasing
// was missed and whose black left-margin bar sits in the middle. Pure and header-only; the shift is a per-picture value kept by the UI.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 {

// A shift wrapped into 0..w-1 (any integer, negative = to the left)
inline int wefaxPhaseWrap(int shift, int w) { return w <= 0 ? 0 : ((shift % w) + w) % w; }

// Every line moves `shift` pixels to the right, wrapping round (a pixel at x lands at (x + shift) mod w). src and out must be different buffers.
template <class T>
void wefaxPhaseShift(const T* src, int w, int h, int shift, std::vector<T>& out) {
    out.assign((size_t)(w > 0 ? w : 0) * (size_t)(h > 0 ? h : 0), T());
    if (w <= 0 || h <= 0 || !src) return;
    const int s = wefaxPhaseWrap(shift, w);
    for (int y = 0; y < h; y++) {
        const T* in = src + (size_t)y * w;
        T* o = out.data() + (size_t)y * w;
        for (int x = 0; x < w; x++) o[(x + s) % w] = in[x];
    }
}

// The shift that moves the darkest vertical bar to the left edge. The bar is the window of max(3, w/100) columns with the lowest mean grey
// (0 black .. 255 white); its left edge ends up at column 0. Returns 0 when the picture is too small or has no contrast (max - min mean < minContrast).
inline int wefaxPhaseAuto(const uint8_t* pix, int w, int h, int minContrast = 30) {
    if (!pix || w < 8 || h < 4) return 0;
    std::vector<double> col((size_t)w, 0.0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) col[(size_t)x] += pix[(size_t)y * w + x];
    for (auto& c : col) c /= h;
    const int k = w / 100 > 3 ? w / 100 : 3;
    double best = 1e18, worst = -1e18; int bestX = 0;
    for (int x = 0; x < w; x++) {
        double s = 0;
        for (int j = 0; j < k; j++) s += col[(size_t)((x + j) % w)];
        s /= k;
        if (s < best) { best = s; bestX = x; }
        if (s > worst) worst = s;
    }
    if (worst - best < minContrast) return 0;
    return wefaxPhaseWrap(-bestX, w);
}

} // namespace dect2
