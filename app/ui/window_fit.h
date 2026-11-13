// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pure logic for floating windows (tested in tests/app/test_gui_rules.cpp): where a window must be moved so that its title bar can
// still be grabbed, and the first-use size of a window on a small display.
#pragma once
#include <algorithm>

namespace winfit {

struct V2 { float x = 0, y = 0; };

// The position that keeps `grab` pixels of the title bar on the display (titleH high) and the bar below the top edge. A window that is
// already reachable is left where it is; one larger than the display is put at the top left corner.
inline V2 reachablePos(V2 pos, V2 size, V2 disp, float grab, float titleH) {
    V2 p = pos;
    const float g = std::min(grab, std::max(1.f, size.x));
    const float minX = g - size.x, maxX = disp.x - g;
    if (maxX >= minX) p.x = std::clamp(p.x, minX, maxX);
    const float maxY = disp.y - std::max(1.f, titleH);
    p.y = std::clamp(p.y, 0.f, std::max(0.f, maxY));
    return p;
}

inline bool moved(V2 a, V2 b) { return a.x != b.x || a.y != b.y; }

// A first-use window size: the wanted size, no larger than the display less a margin on each side
inline V2 firstSize(V2 want, V2 disp, float margin) {
    return V2{std::max(1.f, std::min(want.x, disp.x - 2 * margin)), std::max(1.f, std::min(want.y, disp.y - 2 * margin))};
}

}  // namespace winfit
