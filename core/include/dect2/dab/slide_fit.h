// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB slide show picture in a box (#656): the scale that fits a picture of w x h pixels into a box, never past maxScale (a 320 x 240 slide
// is blown up to 8 times in the pop-out, 4 times inside the page). Box or picture of no size: 0, nothing is drawn.
#pragma once
#include <algorithm>

namespace dect2 {

inline float dabSlideScale(int w, int h, float boxW, float boxH, float maxScale) {
    if (w <= 0 || h <= 0 || boxW <= 0.f || boxH <= 0.f || maxScale <= 0.f) return 0.f;
    return std::min(std::min(boxW / (float)w, boxH / (float)h), maxScale);
}

}   // namespace dect2
