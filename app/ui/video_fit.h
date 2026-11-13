// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// How a picture fills its box (TV player, issue 318). Pure arithmetic, no ImGui: tests/app/test_video_fit.cpp.
#pragma once
namespace vfit {
struct Rect { float w, h, u0, v0, u1, v1; };   // drawn size and the part of the texture shown
// fit: 0 keep the shape, 1 stretch a 4:3 picture to 16:9, 2 zoom (fill the box, crop), 3 stretch to the box
inline Rect rect(float bw, float bh, float ar, int fit) {
    Rect r{bw, bh, 0.f, 0.f, 1.f, 1.f};
    if (bw <= 0 || bh <= 0 || ar <= 0) return r;
    if (fit == 1 && ar < 1.5f) ar = 16.f / 9.f;
    if (fit == 3) return r;
    if (fit == 2) {
        if (bw / bh > ar) { const float vis = bh / (bw / ar); r.v0 = (1.f - vis) * 0.5f; r.v1 = 1.f - r.v0; }
        else { const float vis = bw / (bh * ar); r.u0 = (1.f - vis) * 0.5f; r.u1 = 1.f - r.u0; }
        return r;
    }
    if (bw / bh > ar) r.w = bh * ar; else r.h = bw / ar;
    return r;
}
}
