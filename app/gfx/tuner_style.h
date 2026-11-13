// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The tuner look "Simple" (Settings > Display > Tuner style; 0 is the classic band drawn by plots.cpp): a red centre line and two grey lines at
// the passband edges, over the spectrum and the waterfall, clipped to the plot. Only drawing: plots.cpp passes the plot's rectangle and the pixel columns.
#pragma once
#include "imgui.h"
#include <algorithm>
#include <cmath>

namespace tstyle {

struct In {
    ImVec2 pmin, psize;                // the plot area on the screen
    double xLo = 0, xHi = 0, xC = 0;   // the band's edges and the tuned frequency, in screen pixels
    bool dragging = false;             // an edge is being dragged
    int hover = -1;                    // the edge under the mouse
    float sc = 1.f;                    // interface scale
};

inline void draw(ImDrawList* dl, const In& I) {
    if (I.psize.x <= 0 || I.psize.y <= 0) return;
    const float y0 = I.pmin.y, y1 = I.pmin.y + I.psize.y;
    dl->PushClipRect(I.pmin, ImVec2(I.pmin.x + I.psize.x, y1), true);
    const float xc = std::floor((float)I.xC) + 0.5f;
    if (I.xHi - I.xLo >= 4.0) {   // edges closer than 2 px to the centre line: only the red line
        const float xl = std::floor((float)I.xLo) + 0.5f, xh = std::floor((float)I.xHi) + 0.5f;
        dl->AddRectFilled(ImVec2(xl, y0), ImVec2(xh, y1), IM_COL32(255, 255, 255, 8));   // almost invisible
        for (int e = 0; e < 2; e++) {
            const bool hot = I.dragging || I.hover == e;
            dl->AddLine(ImVec2(e ? xh : xl, y0), ImVec2(e ? xh : xl, y1), hot ? IM_COL32(185, 187, 190, 255) : IM_COL32(135, 138, 142, 235), 1.f);
        }
    }
    dl->AddLine(ImVec2(xc, y0), ImVec2(xc, y1), IM_COL32(232, 48, 44, 255), 1.5f);
    dl->PopClipRect();
}

}   // namespace tstyle
