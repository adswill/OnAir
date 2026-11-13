// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "ui/video_fit.h"
#include <cmath>
#include <cstdio>
static bool eq(float a, float b) { return std::fabs(a - b) < 1e-4f; }
int main() {
    int f = 0;
    auto chk = [&](bool ok, const char* m) { if (!ok) { std::printf("FAIL: %s\n", m); f++; } };
    auto r = vfit::rect(1600, 900, 4.f / 3, 0);          // fit: pillarbox
    chk(eq(r.h, 900) && eq(r.w, 1200), "fit 4:3 in 16:9");
    r = vfit::rect(1600, 900, 4.f / 3, 1);               // stretch: full box
    chk(eq(r.w, 1600) && eq(r.h, 900) && eq(r.u0, 0) && eq(r.v0, 0), "stretch 4:3 to 16:9");
    r = vfit::rect(1600, 900, 16.f / 9, 1);              // a 16:9 picture is left alone
    chk(eq(r.w, 1600) && eq(r.h, 900), "stretch leaves 16:9");
    r = vfit::rect(1600, 900, 4.f / 3, 2);               // zoom: crop top and bottom
    chk(eq(r.w, 1600) && eq(r.h, 900) && eq(r.u0, 0) && eq(r.v0, 0.125f) && eq(r.v1, 0.875f), "zoom crops 4:3 height");
    r = vfit::rect(900, 900, 16.f / 9, 2);               // wide picture in a square: crop the sides
    chk(eq(r.v0, 0) && eq(r.u0, (1 - 0.5625f) * 0.5f), "zoom crops 16:9 sides");
    r = vfit::rect(800, 600, 4.f / 3, 3);
    chk(eq(r.w, 800) && eq(r.h, 600), "fill");
    return f ? 1 : 0;
}
