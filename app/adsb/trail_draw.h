// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Trail drawing for the aircraft map and the radar: which segments of a projected track are worth drawing. Pure (tests/app/test_render_caps.cpp).
// - decimation: a point closer than minPx to the last point drawn is skipped (the newest point is always kept), so a zoomed-out map with
//   hundreds of 80-point trails does not draw tens of thousands of sub-pixel lines
// - culling: a segment with both ends beyond the same edge of the view cannot cross it and is not drawn
#pragma once
#include <cstddef>

namespace trails {

struct Pt { float x, y; };
struct Rect { float x0, y0, x1, y1; };

inline bool sameSideOutside(Pt a, Pt b, const Rect& r) {
    return (a.x < r.x0 && b.x < r.x0) || (a.x > r.x1 && b.x > r.x1) || (a.y < r.y0 && b.y < r.y0) || (a.y > r.y1 && b.y > r.y1);
}

// Calls emit(from, to) with point indices for each segment to draw, oldest first. Returns the number emitted.
template <class Emit>
size_t forEachSegment(const Pt* p, size_t n, float minPx, const Rect& clip, Emit&& emit) {
    if (n < 2) return 0;
    const float min2 = minPx * minPx;
    size_t last = 0, count = 0;
    for (size_t i = 1; i < n; i++) {
        const float dx = p[i].x - p[last].x, dy = p[i].y - p[last].y;
        if (i + 1 < n && dx * dx + dy * dy < min2) continue;
        if (!sameSideOutside(p[last], p[i], clip)) { emit(last, i); count++; }
        last = i;
    }
    return count;
}

}   // namespace trails
