// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Where the right-hand buttons of the classic top bar go (radio settings, support, version, clear all, language, server, new interface, touch,
// tour). They were put at fixed offsets from the window's right edge, and the language group at "right edge minus a width that left out the
// Touch button". On a narrow window (640 px on a Pi, #139) or a wide mode selector (#424) the buttons landed on top of the mode selector and
// of each other. ImGui gives a click to the item drawn first, so the buttons on top did nothing. This lays them out with no overlap: on the
// selector's line, right-aligned, when they fit after it; otherwise on the next line(s), wrapping where a button would pass the right edge.
// Pure function, tested in tests/app/test_top_row.cpp.
#pragma once
#include <cstddef>
#include <vector>

namespace toprow {

struct Place {
    bool newLine = false;   // the button starts a line below the previous control
    float x = 0;            // its left edge (window coordinates)
};

// w: each button's width. leftEnd: where the controls left of the buttons end on the first line. lineStart: x of a new line. right: the
// right edge the buttons must stay inside. sp: the space between two buttons. gap: the least space between the left controls and the buttons.
inline std::vector<Place> layout(const std::vector<float>& w, float leftEnd, float lineStart, float right, float sp, float gap) {
    std::vector<Place> out(w.size());
    if (w.empty()) return out;
    float total = 0;
    for (size_t i = 0; i < w.size(); i++) total += w[i] + (i ? sp : 0);
    float x;
    if (leftEnd + gap + total <= right) {          // all on the selector's line, right-aligned
        x = right - total;
        out[0].newLine = false;
    } else {                                        // a line of their own: right-aligned when they fit in one line, else from the left
        x = total <= right - lineStart ? right - total : lineStart;
        out[0].newLine = true;
    }
    out[0].x = x;
    x += w[0];
    for (size_t i = 1; i < w.size(); i++) {
        const float nx = x + sp;
        if (nx + w[i] > right) {   // past the edge: the next line (a button wider than the window still gets a line of its own)
            out[i].newLine = true; out[i].x = lineStart; x = lineStart + w[i]; continue;
        }
        out[i].x = nx;
        x = nx + w[i];
    }
    return out;
}

} // namespace toprow
