// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The adaptive layout of the main area in the new interface (sidebar layout, ui2.cpp bodyParent; docs/LAYOUT-4.0.md): the spectrum and
// waterfall pane, the mode's list on the right ("results column") and its analysis row at the bottom (constellationsTab). Every mode used
// to get all three at fixed sizes, so a mode with nothing in the lower row showed a large empty box, and a list of one status line kept a
// full-height column. Here each panel is sized to what it drew on the last frame:
//   - the lower row is hidden when it drew nothing, shrinks to its content when that is shorter than the row, and moves into the right
//     column (under the list) when its content is narrow enough to fit there (a single 140 px constellation box);
//   - a list of at most about three lines becomes a strip above the spectrum, so the spectrum gets the column's width;
//   - a change waits until it has held for a moment (show 0.25 s, hide 0.6 s) and at least 1 s after the previous one, then the sizes
//     glide to the new place in about 0.15 s: content that flickers on and off does not make the layout jump every frame;
//   - a size the user dragged for this mode is kept: a dragged row stays a row at that height, a dragged column stays a column.
// Pure: no ImGui, so tests/app/test_overview_layout.cpp can drive it with any window size and content. Sizes are pixels; boxes are relative
// to the top left of the area the three panels share.
#pragma once
#include <algorithm>
#include <cmath>

namespace ovl {

struct Box {
    float x = 0, y = 0, w = 0, h = 0;
    float area() const { return w > 0 && h > 0 ? w * h : 0.f; }
    bool shown() const { return w > 0.5f && h > 0.5f; }
};

// what a panel drew on the last frame, from its first cursor position to the furthest item (padding not included); negative: not measured
struct Extent {
    float w = -1, h = -1;
    bool valid() const { return w >= 0 && h >= 0; }
    bool empty() const { return valid() && (w < 1.f || h < 1.f); }
};

enum class Bottom { Row, Column, Hidden };
enum class Right { Column, Strip };

struct In {
    float W = 0, H = 0;            // the area of the three panels
    float gap = 4;                 // between panels
    float lineH = 17;              // one text line with its spacing
    float padX = 16, padY = 10;    // a panel's padding and border, both sides together
    double now = 0;                // seconds (any clock that moves forward)
    bool adaptive = true;          // false: the fixed layout of 3.9 (View > Fit panels to their content off)
    bool bottomAllowed = true;     // false: no lower row in this view (ATSC 3.0, the transmit view)
    float rightAutoW = 300, bottomAutoH = 200;   // the automatic sizes
    float userRightW = 0, userBottomH = 0;       // sizes the user dragged for this mode (0: none)
    float minMainW = 260, minMainH = 186, minRightW = 150, minBottomH = 90;
    Extent right;                  // the list, measured in the last layout's right box, or in probeRightBox when probeRight was set
    Extent bottom;                 // the lower content, measured in the last layout's bottom box (or probeBottom)
};

struct Out {
    Box main, right, bottom, strip;          // a box with no size is not drawn; strip: the list as a strip above the spectrum
    Bottom bottomAt = Bottom::Row;
    Right rightAt = Right::Column;
    bool probeRight = false, probeBottom = false;   // draw that panel once more, out of sight, in the box below, to measure it
    Box probeRightBox, probeBottomBox;
    bool animating = false;                  // the sizes are still gliding: keep redrawing
};

struct State {
    Bottom bottom = Bottom::Row, pendB = Bottom::Row;
    Right right = Right::Column, pendR = Right::Column;
    double pendBSince = -1, pendRSince = -1, lastSwitch = -1e9, born = -1, lastNow = -1;
    float natBW = -1, natBH = -1;            // the lower content's natural size (measured in a full row); 0: it drew nothing
    float dispBW = -1, dispBH = -1;          // the last natural size it had while it drew something: the boxes keep it while a hide waits
    float natRH = -1;                        // the list's natural height (measured in a column)
    float rowShown = -1, colShown = -1;      // the sizes on screen while they glide
    Out last;
    bool hasLast = false;
};

constexpr double kShowHold = 0.25, kHideHold = 0.6, kDwell = 1.0, kSettle = 0.35, kGlide = 0.15;

inline float thinLimit(const In& in) { return in.lineH * 3.2f; }   // a list this short is a strip

inline float clampf(float v, float lo, float hi) { return std::max(lo, std::min(std::max(lo, hi), v)); }

// the column width and the full row height for these inputs (the user's sizes, else the automatic ones, kept inside the area)
inline float fullColW(const In& in) { return std::floor(clampf(in.userRightW > 0 ? in.userRightW : in.rightAutoW, in.minRightW, in.W - in.gap - in.minMainW)); }
inline float fullRowH(const In& in) { return std::floor(clampf(in.userBottomH > 0 ? in.userBottomH : in.bottomAutoH, in.minBottomH, in.H - in.gap - in.minMainH)); }

// the fixed layout (3.9): main | list, then the row under both
inline Out fixedLayout(const In& in) {
    Out o;
    const float colW = fullColW(in), rowH = in.bottomAllowed ? fullRowH(in) : 0.f;
    const float topH = in.H - (rowH > 0 ? rowH + in.gap : 0.f);
    o.main = {0, 0, in.W - colW - in.gap, topH};
    o.right = {in.W - colW, 0, colW, topH};
    if (rowH > 0) o.bottom = {0, in.H - rowH, in.W, rowH};
    o.bottomAt = rowH > 0 ? Bottom::Row : Bottom::Hidden;
    return o;
}

namespace detail {

// takes in the measurements of the last frame: the natural sizes of the two panels
inline void measure(const In& in, State& st) {
    if (!st.hasLast) {   // the first frame: whatever was measured, in the boxes of the fixed layout
        if (in.bottom.valid()) { st.natBW = in.bottom.w; st.natBH = in.bottom.h; }
        if (!in.bottom.empty() && in.bottom.valid()) { st.dispBW = st.natBW; st.dispBH = st.natBH; }
        if (in.right.valid()) st.natRH = in.right.h;
        return;
    }
    const Out& L = st.last;
    if (in.right.valid()) {   // the list is always measured in a full column box (the real one, or the probe while it is a strip or cut short)
        const Box& b = L.probeRight ? L.probeRightBox : L.right;
        if (!L.animating || in.right.h > b.h - in.padY + 2) st.natRH = in.right.h;
    }
    if (in.bottom.valid()) {
        if (in.bottom.empty()) { st.natBW = 0; st.natBH = 0; return; }   // dispBW / dispBH stay: the box does not shrink while it waits
        const bool probe = L.probeBottom;   // else it was measured in L.bottom (a row gliding away included)
        const Box& b = probe ? L.probeBottomBox : L.bottom;
        const bool fullBox = probe || (L.bottomAt == Bottom::Row && b.h >= fullRowH(in) - 0.5f);
        if (fullBox && !L.animating) { st.natBW = in.bottom.w; st.natBH = in.bottom.h; }
        else {   // a reduced box (a shrunk row, the column): only growth counts, so content that follows its box does not ratchet down
            if (in.bottom.w > b.w - in.padX + 2) st.natBW = in.bottom.w;
            if (in.bottom.h > b.h - in.padY + 2) st.natBH = in.bottom.h;
            if (st.natBW < 1 || st.natBH < 1) { st.natBW = in.bottom.w; st.natBH = in.bottom.h; }   // back after drawing nothing
        }
        st.dispBW = st.natBW; st.dispBH = st.natBH;
    }
}

inline float rightMinH(const In& in) { return in.lineH * 4 + in.padY; }   // the list keeps at least four lines beside a plot in the column

inline Bottom wantBottom(const In& in, const State& st) {
    if (!in.bottomAllowed) return Bottom::Hidden;
    if (!in.adaptive) return Bottom::Row;
    if (st.natBW < 0 || st.natBH < 0) return st.bottom;   // not measured yet
    if (st.natBW < 1 || st.natBH < 1) return Bottom::Hidden;
    if (in.userBottomH > 0) return Bottom::Row;
    const float colW = fullColW(in);
    const float margin = st.bottom == Bottom::Column ? 0.f : 8.f;   // a band, so a size at the limit does not flip the place
    const bool narrow = st.natBW + in.padX <= colW - margin;
    const bool fitsH = st.natBH + in.padY <= std::min(in.H * 0.6f, in.H - in.gap - rightMinH(in));
    return narrow && fitsH ? Bottom::Column : Bottom::Row;
}

inline Right wantRight(const In& in, const State& st, Bottom b) {
    if (!in.adaptive || in.userRightW > 0 || b == Bottom::Column) return Right::Column;
    if (st.natRH < 0) return st.right;
    const float lim = thinLimit(in) * (st.right == Right::Strip ? 1.35f : 1.f);
    return st.natRH <= lim ? Right::Strip : Right::Column;
}

inline bool due(double now, double since, double hold, const State& st) {
    return now - since >= hold && now - st.lastSwitch >= kDwell;
}

inline float glide(float shown, float target, double dt) {
    if (shown < 0 || dt <= 0 || dt > 0.25) return target;
    const float k = (float)std::min(1.0, dt / kGlide * 2.5);   // about 92 % of the way in kGlide
    const float v = shown + (target - shown) * k;
    return std::fabs(v - target) < 0.75f ? target : v;
}

}  // namespace detail

inline Out layout(const In& in, State& st) {
    if (!in.adaptive) {   // the fixed layout, and forget what was measured (it was measured in other boxes)
        st = State();
        Out o = fixedLayout(in);
        return o;
    }
    if (st.born < 0) st.born = in.now;
    detail::measure(in, st);
    const bool settling = in.now - st.born < kSettle;   // a mode just opened: go straight to its place, no waiting
    // ---- the places, with hysteresis
    const Bottom tb = detail::wantBottom(in, st);
    if (tb != st.bottom) {
        const bool now = settling || !in.bottomAllowed;
        if (st.pendB != tb || st.pendBSince < 0) { st.pendB = tb; st.pendBSince = in.now; }
        if (now || detail::due(in.now, st.pendBSince, tb == Bottom::Hidden ? kHideHold : kShowHold, st)) {
            st.bottom = tb; st.pendBSince = -1; if (!now) st.lastSwitch = in.now;
        }
    } else st.pendBSince = -1;
    const Right tgtR = detail::wantRight(in, st, st.bottom);
    if (tgtR != st.right) {
        const bool now = settling || (tgtR == Right::Column && st.bottom == Bottom::Column);   // the column is needed for the plot at once
        if (st.pendR != tgtR || st.pendRSince < 0) { st.pendR = tgtR; st.pendRSince = in.now; }
        if (now || detail::due(in.now, st.pendRSince, tgtR == Right::Strip ? kHideHold : kShowHold, st)) {
            st.right = tgtR; st.pendRSince = -1; if (!now) st.lastSwitch = in.now;
        }
    } else st.pendRSince = -1;

    // ---- the target sizes
    const float colFull = fullColW(in), rowFull = fullRowH(in);
    const bool thinList = st.natRH >= 0 && st.natRH <= thinLimit(in);
    const bool column = st.right == Right::Column || st.bottom == Bottom::Column;
    float colT = 0;
    if (column) {
        colT = colFull;
        // a short list over a small plot: the column only as wide as the plot (the spectrum takes the rest)
        if (st.bottom == Bottom::Column && thinList && in.userRightW <= 0) colT = std::floor(clampf(st.dispBW + in.padX, in.minRightW, colFull));
    }
    float rowT = 0;
    if (st.bottom == Bottom::Row) {
        rowT = rowFull;
        if (in.userBottomH <= 0 && st.dispBH >= 1) rowT = std::floor(clampf(st.dispBH + in.padY, in.minBottomH, rowFull));   // shorter content: a shorter row
    }
    const double dt = st.lastNow < 0 ? 0 : in.now - st.lastNow;
    st.lastNow = in.now;
    st.colShown = settling ? colT : detail::glide(st.colShown, colT, dt);
    st.rowShown = settling ? rowT : detail::glide(st.rowShown, rowT, dt);
    const float colW = st.colShown, rowH = st.rowShown;

    // ---- the boxes
    Out o;
    o.bottomAt = st.bottom;
    o.rightAt = st.right;
    o.animating = colW != colT || rowH != rowT;
    const float topH = in.H - (rowH > 0.5f ? rowH + in.gap : 0.f);
    const float mainW = in.W - (colW > 0.5f ? colW + in.gap : 0.f);
    float mainY = 0;
    if (st.right == Right::Strip && st.natRH >= 1) {   // a list that drew nothing gets no strip at all (it is still measured)
        const float sh = std::floor(std::min(std::max(st.natRH, in.lineH), thinLimit(in) * 1.35f) + in.padY);
        o.strip = {0, 0, mainW, sh};
        mainY = sh + in.gap;
    }
    o.main = {0, mainY, mainW, std::max(0.f, topH - mainY)};
    if (colW > 0.5f) {
        const float x = in.W - colW;
        if (st.bottom == Bottom::Column) {
            const float bh = std::floor(std::max(0.f, std::min(std::max(0.f, st.dispBH) + in.padY, topH - in.gap - detail::rightMinH(in))));
            if (st.right == Right::Column && !(thinList && st.natRH < 1)) {   // (a list that drew nothing: the plot alone)
                const float rh = thinList ? std::min(st.natRH + in.padY, topH - bh - in.gap) : topH - bh - in.gap;
                o.right = {x, 0, colW, std::floor(rh)};
                o.bottom = {x, o.right.h + in.gap, colW, bh};
            } else o.bottom = {x, 0, colW, bh};
        } else if (st.right == Right::Column) o.right = {x, 0, colW, topH};
    }
    if (rowH > 0.5f) o.bottom = {0, in.H - rowH, in.W, rowH};
    // ---- what has to be measured out of sight
    o.probeBottom = in.bottomAllowed && st.bottom == Bottom::Hidden && rowH <= 0.5f;
    if (o.probeBottom) o.probeBottomBox = {0, in.H - rowFull, in.W, rowFull};
    o.probeRight = st.right == Right::Strip || (colW > 0.5f && thinList && o.right.h < topH - 0.5f);   // cut short over a plot: a list that grows would not show it
    if (o.probeRight) o.probeRightBox = {in.W - colFull, 0, colFull, topH};
    st.last = o;
    st.hasLast = true;
    return o;
}

// the share of the area that shows nothing: the whole area minus the spectrum pane (the spectrum and waterfall fill it) and minus what the
// list and the lower content drew in their boxes. The gaps between panels count as empty, in both layouts alike.
inline float unusedShare(const In& in, const Out& o, float natRightH, float natBottomW, float natBottomH) {
    const float total = in.W * in.H;
    if (total <= 0) return 0;
    float used = o.main.area();
    const float rh = std::max(0.f, natRightH) + in.padY;
    if (o.right.shown()) used += o.right.w * std::min(o.right.h, rh);
    if (o.strip.shown()) used += o.strip.w * std::min(o.strip.h, rh);
    if (o.bottom.shown() && natBottomW > 0 && natBottomH > 0) used += std::min(o.bottom.w, natBottomW + in.padX) * std::min(o.bottom.h, natBottomH + in.padY);
    return std::max(0.f, total - used) / total;
}

// the spectrum pane's share of the area
inline float mainShare(const In& in, const Out& o) { return in.W * in.H > 0 ? o.main.area() / (in.W * in.H) : 0.f; }

}  // namespace ovl
