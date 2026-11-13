// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The waterfall at the size it is shown. The history is 4096 columns by 512 rows; drawn straight onto a plot of ~1000 x ~400 pixels, the GPU
// (linear filter, no mipmaps) picked about one texel in four across and some of the rows down, and as the picture slid down by part of a row
// each frame it picked other rows: narrow or short signals blinked. Here each pixel column is the highest level of the columns it covers, and
// the rows go in groups of k (a power of two), the highest of each, so a group is at least a pixel tall: the picture is then only ever
// magnified, and a group keeps its look while it slides. Pure (no ImGui, no GPU): tested in tests/app/test_plot_util.cpp.
#pragma once
#include "spec_look.h"
#include "spec_pipe.h"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace plt {

// One history for the pixel-exact view (4.0, updateExact): W columns per row over [lo, hi) MHz. The zoom history (zoom_detail.h) also has ok
// (a flag per ring row: that row has a zoom-spectrum row) and the band it can be trusted over [useLo, useHi]; gen changes when its rows move.
struct WfSrc {
    const uint8_t* lv = nullptr;
    int W = 0;
    double lo = 0, hi = 0;
    const uint8_t* ok = nullptr;
    double useLo = 0, useHi = 0;
    int gen = 0;
};

struct WfView {
    int c0 = 0, c1 = 0, cols = 0, k = 0, ring = 0;   // source columns [c0, c1) onto cols pixel columns; k rows a group, ring groups kept
    // The texture is drawn with a linear filter, so its edges blend with the texel beyond them: past the last column, and below row
    // ring - 1 where the picture goes on at row 0. Unwritten (black) texels there were a dark line that slid with the picture. So px has
    // one more column (a copy of the last) and one more row (a copy of row 0) wherever the texture has room (W x H).
    int stride = 0, rowsAlloc = 0;
    long long newest = -1, lastR = -1;               // the newest group built, and the newest row it had
    int lutGen = -1;
    // The look of the view (the spectrum look sets it): Smooth makes a zoomed-in view a pixel a column with the levels interpolated between
    // the history columns (the map then colours the in-between level: no stair steps, and no muddy blend of two colours as the GPU's
    // linear filter gives); LowPass is the 2D noise reduction ([1 2 1] down and across). 0: as it always was.
    enum Mode { Smooth = 1, LowPass = 2 };
    int mode = 0, builtMode = 0;
    std::vector<uint32_t> px;   // rowsAlloc rows of stride colours; group g in row rowOf(g) (newer groups above, wrapping, as the history)
    std::vector<uint8_t> acc, lp;
    // The pixel-exact view (4.0, docs/WATERFALL-4.0.md): px columns are the plot's framebuffer pixels over exactly [vLo, vHi], each from the
    // history columns under it (spipe::pixelLevels: the peak where columns are finer, sharp steps where they are coarser), so the GPU never
    // stretches the picture across: no blur when zoomed in. Rows that the zoom history has (det) come from it, at its finer resolution.
    bool exact = false, detOn = false;
    double vLo = 0, vHi = 0, mLo = 0, mHi = 0;
    int mW = 0, detGen = -1;
    std::vector<uint8_t> rowA, rowB, rowC;

    // As update(), for the view [visLo, visHi] over ncols pixels from main (and det where it has rows and the view is inside its band).
    bool updateExact(const WfSrc& main, const WfSrc* det, int H, int wr, long long R, int have, const uint32_t* lut, int gen,
                     double visLo, double visHi, int ncols, int nk, bool& full, std::vector<int>& dirty) {
        dirty.clear(); full = false;
        nk = std::max(1, std::min(H, nk));
        if (!main.lv || main.W < 1 || !(visHi > visLo) || ncols < 1 || R < 0) return false;
        const bool dOn = det && det->lv && det->ok && det->W > 0 && visLo >= det->useLo && visHi <= det->useHi;
        const long long gN = R / nk;
        const bool same = exact && visLo == vLo && visHi == vHi && ncols == cols && nk == k && gen == lutGen && mode == builtMode && newest >= 0
                          && dOn == detOn && (!dOn || det->gen == detGen) && main.W == mW && main.lo == mLo && main.hi == mHi;
        if (!same || gN - newest >= ring) {
            exact = true; vLo = visLo; vHi = visHi; cols = ncols; k = nk; ring = ringFor(k, H); lutGen = gen; builtMode = mode;
            detOn = dOn; detGen = dOn ? det->gen : -1; mW = main.W; mLo = main.lo; mHi = main.hi;
            c0 = 0; c1 = main.W;
            stride = cols + 1; rowsAlloc = ring + (ring < H ? 1 : 0);
            px.assign((size_t)rowsAlloc * stride, 0xFF000000u);
            for (long long g = std::max(0LL, gN - ring + 1); g <= gN; g++) buildExact(main, dOn ? det : nullptr, H, wr, R, have, lut, g);
            newest = gN; lastR = R; full = true;
            return true;
        }
        if (R == lastR) return false;
        for (long long g = (mode & LowPass) && newest > 0 && gN - (newest - 1) < ring ? newest - 1 : newest; g <= gN; g++) {
            buildExact(main, dOn ? det : nullptr, H, wr, R, have, lut, g); dirty.push_back(rowOf(g));
            if (rowOf(g) == 0 && rowsAlloc > ring) dirty.push_back(ring);
        }
        // the oldest group kept loses its oldest rows as the ring overwrites them: made again so it shows what a rebuild would
        if (const long long g0 = gN - ring + 1; g0 >= 0 && g0 < newest) {
            buildExact(main, dOn ? det : nullptr, H, wr, R, have, lut, g0); dirty.push_back(rowOf(g0));
            if (rowOf(g0) == 0 && rowsAlloc > ring) dirty.push_back(ring);
        }
        newest = gN; lastR = R;
        return true;
    }

    // The rows a group needs so that rowsShown rows over heightPx pixels are never squeezed (a power of two, at most H).
    static int groupRows(double rowsShown, double heightPx, int H) {
        int k = 1;
        while (k < H && rowsShown > heightPx * k) k *= 2;
        return k;
    }
    // Groups kept: the newest one may be partly filled, so one more than H / k (which covers H rows)
    static int ringFor(int k, int H) { return k <= 1 ? H : H / k + 1; }
    int rowOf(long long g) const { return ring - 1 - (int)(g % ring); }

    // lv: H rows of W levels (0..255, the index into lut); the newest row, number R (counted from the first), at row wr, older ones at
    // wr+1, wr+2 ... wrapping; have: how many rows there are. Brings px up to date for the columns [nc0, nc1) on ncols pixels in groups of
    // nk rows. full: every row of px changed (upload stride x rowsAlloc); else dirty lists the rows that did. False when nothing changed.
    bool update(const uint8_t* lv, int W, int H, int wr, long long R, int have, const uint32_t* lut, int gen,
                int nc0, int nc1, int ncols, int nk, bool& full, std::vector<int>& dirty) {
        dirty.clear(); full = false;
        nc0 = std::max(0, std::min(W, nc0)); nc1 = std::max(nc0, std::min(W, nc1));
        nk = std::max(1, std::min(H, nk));
        if (nc1 <= nc0 || ncols < 1 || R < 0) return false;
        if (!(mode & Smooth)) ncols = std::min(ncols, nc1 - nc0);
        const long long gN = R / nk;
        const bool same = !exact && nc0 == c0 && nc1 == c1 && ncols == cols && nk == k && gen == lutGen && newest >= 0 && mode == builtMode;
        if (!same || gN - newest >= ring) {
            c0 = nc0; c1 = nc1; cols = ncols; k = nk; ring = ringFor(k, H); lutGen = gen; builtMode = mode; exact = false;
            stride = cols + (cols < W ? 1 : 0); rowsAlloc = ring + (ring < H ? 1 : 0);
            px.assign((size_t)rowsAlloc * stride, 0xFF000000u);
            for (long long g = std::max(0LL, gN - ring + 1); g <= gN; g++) build(lv, W, H, wr, R, have, lut, g);
            newest = gN; lastR = R; full = true;
            return true;
        }
        if (R == lastR) return false;
        // the low-pass reads the newer neighbour row too: the group before the newest one changes when the next group starts
        for (long long g = (mode & LowPass) && newest > 0 && gN - (newest - 1) < ring ? newest - 1 : newest; g <= gN; g++) {
            build(lv, W, H, wr, R, have, lut, g); dirty.push_back(rowOf(g));
            if (rowOf(g) == 0 && rowsAlloc > ring) dirty.push_back(ring);
        }
        newest = gN; lastR = R;
        return true;
    }

private:
    // row r (counted from the first) as pixel levels into out; false when the history does not have it
    bool pixRow(const WfSrc& main, const WfSrc* det, int H, int wr, long long R, int have, long long r, std::vector<uint8_t>& out) {
        if (r < 0 || r > R || R - r >= have) return false;
        const size_t ri = (size_t)((wr + (R - r)) % H);
        out.resize((size_t)cols);
        if (det && det->ok[ri]) spipe::pixelLevels(det->lv + ri * (size_t)det->W, det->W, det->lo, det->hi, vLo, vHi, cols, out.data(), 0);
        else spipe::pixelLevels(main.lv + ri * (size_t)main.W, main.W, main.lo, main.hi, vLo, vHi, cols, out.data(), 0);
        return true;
    }
    void buildExact(const WfSrc& main, const WfSrc* det, int H, int wr, long long R, int have, const uint32_t* lut, long long g) {
        acc.assign((size_t)cols, 0);
        bool any = false;
        for (long long r = g * k; r < (g + 1) * k; r++) {
            if (!pixRow(main, det, H, wr, R, have, r, rowB)) continue;
            const uint8_t* s = rowB.data();
            if (mode & LowPass) {   // the row with its neighbours, down and across, on the pixel rows
                const bool hn = pixRow(main, det, H, wr, R, have, r + 1, rowA), ho = pixRow(main, det, H, wr, R, have, r - 1, rowC);
                lp.resize((size_t)cols);
                slook::lowpassRow(hn ? rowA.data() : nullptr, s, ho ? rowC.data() : nullptr, cols, lp.data());
                s = lp.data();
            }
            for (int c = 0; c < cols; c++) acc[(size_t)c] = std::max(acc[(size_t)c], s[c]);
            any = true;
        }
        uint32_t* o = px.data() + (size_t)rowOf(g) * stride;
        if (!any) std::fill(o, o + stride, 0xFF000000u);
        else {
            for (int x = 0; x < cols; x++) o[x] = lut[acc[(size_t)x]];
            o[cols] = o[cols - 1];
        }
        if (rowOf(g) == 0 && rowsAlloc > ring) std::copy(o, o + stride, px.data() + (size_t)ring * stride);
    }
    void build(const uint8_t* lv, int W, int H, int wr, long long R, int have, const uint32_t* lut, long long g) {
        const int span = c1 - c0;
        acc.assign((size_t)span, 0);
        bool any = false;
        auto rowPtr = [&](long long r) -> const uint8_t* { return r < 0 || r > R || R - r >= have ? nullptr : lv + (size_t)((wr + (R - r)) % H) * W + c0; };
        for (long long r = g * k; r < (g + 1) * k; r++) {
            const uint8_t* s = rowPtr(r);
            if (!s) continue;
            if (mode & LowPass) {   // the row with its neighbours, down and across
                lp.resize((size_t)span);
                slook::lowpassRow(rowPtr(r + 1), s, rowPtr(r - 1), span, lp.data());
                s = lp.data();
            }
            for (int c = 0; c < span; c++) acc[c] = std::max(acc[c], s[c]);
            any = true;
        }
        uint32_t* o = px.data() + (size_t)rowOf(g) * stride;
        if (!any) std::fill(o, o + stride, 0xFF000000u);   // no rows yet: black, as the history starts
        else {
            if ((mode & Smooth) && cols > span) {   // zoomed in: each pixel the level between the two columns it falls between
                for (int x = 0; x < cols; x++) {
                    const double f = ((double)x + 0.5) * span / cols - 0.5;
                    const int i = std::max(0, std::min(span - 1, (int)std::floor(f)));
                    const int j = std::min(span - 1, i + 1);
                    o[x] = lut[slook::lerpLevel(acc[i], acc[j], (float)(f - i))];
                }
            } else {
                for (int x = 0; x < cols; x++) {
                    const int a = (int)((long long)span * x / cols), b = std::max(a + 1, (int)((long long)span * (x + 1) / cols));
                    uint8_t m = acc[a];
                    for (int c = a + 1; c < b; c++) m = std::max(m, acc[c]);
                    o[x] = lut[m];
                }
            }
            if (stride > cols) o[cols] = o[cols - 1];
        }
        if (rowOf(g) == 0 && rowsAlloc > ring) std::copy(o, o + stride, px.data() + (size_t)ring * stride);
    }
};

} // namespace plt
