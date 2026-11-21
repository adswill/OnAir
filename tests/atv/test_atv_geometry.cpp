// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Picture geometry of the analogue TV chain (Discord report: the decoded picture looked squashed vertically, with black bars above and below).
// A picture with a known shape (a disc on a grey ground, round in the 4:3 frame the generator is given) goes through generator -> receiver for every
// line standard. The decoded frame must be 4:3, the disc must come out round (width / height within 3 %), and the grey ground must reach the top and
// the bottom edge of the picture (no bars made by blanking lines being counted as picture).
#include "dect2/atv/atv_testkit.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
using namespace dect2;
using namespace dect2::atvkit;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static double lumaAt(const AtvFrame& f, int x, int y) {
    const uint8_t* p = &f.rgba[((size_t)y * (size_t)f.width + (size_t)x) * 4];
    return (p[0] + p[1] + p[2]) / (3.0 * 255.0);
}

int main() {
    struct Sys { int sys; const char* name; };
    const Sys systems[] = {{kAtvG, "625 PAL-G"}, {kAtvM, "525 M"}, {kAtvA, "405 A"}, {kAtvE, "819 E"}, {kAtvL, "625 L"}};
    for (const Sys& s : systems) {
        AtvGenConfig c;
        c.sys = s.sys; c.colour = kAtvMono; c.cnrDb = 50;
        c.pullVideo = [](double, int w, int h, float* y, float* u, float* v) {
            const double cx = w * 0.5, cy = h * 0.5, r = h * 0.3;
            for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
                const double dx = i + 0.5 - cx, dy = j + 0.5 - cy;
                const size_t o = (size_t)j * w + i;
                y[o] = dx * dx + dy * dy < r * r ? 0.9f : 0.3f; u[o] = 0; v[o] = 0;
            }
            return true;
        };
        const Run r = run(c, 2.5);
        if (!r.frame) { CHECK(false, "%s: no picture", s.name); continue; }
        const AtvFrame& f = *r.frame;
        CHECK(std::fabs((double)f.width / f.height - 4.0 / 3) < 0.02, "%s: frame %d x %d is not 4:3", s.name, f.width, f.height);
        // the ground reaches the first and last rows and columns
        const double top = lumaAt(f, f.width / 8, 2), bot = lumaAt(f, f.width / 8, f.height - 3), left = lumaAt(f, 2, f.height / 8), right = lumaAt(f, f.width - 3, f.height / 8);
        CHECK(top > 0.15 && bot > 0.15 && left > 0.15 && right > 0.15, "%s: bars at the edges: top %.2f bottom %.2f left %.2f right %.2f", s.name, top, bot, left, right);
        int minx = f.width, maxx = -1, miny = f.height, maxy = -1;
        for (int y = 0; y < f.height; y++) for (int x = 0; x < f.width; x++) if (lumaAt(f, x, y) > 0.6) { minx = std::min(minx, x); maxx = std::max(maxx, x); miny = std::min(miny, y); maxy = std::max(maxy, y); }
        if (maxx < 0) { CHECK(false, "%s: disc not found", s.name); continue; }
        const double w = maxx - minx + 1, h = maxy - miny + 1;
        const double ratio = w / h;   // a 4:3 frame has square pixels: the disc is round when this is 1
        printf("%s: frame %dx%d, disc %.0f x %.0f, ratio %.3f, height %.3f of the frame (want 0.6)\n", s.name, f.width, f.height, w, h, ratio, h / f.height);
        CHECK(std::fabs(ratio - 1.0) < 0.03, "%s: disc %.0f x %.0f is squashed (ratio %.3f)", s.name, w, h, ratio);
        CHECK(std::fabs(h / f.height - 0.6) < 0.03, "%s: disc height %.3f of the frame, want 0.6", s.name, h / f.height);
    }
    printf(fails ? "atv_geometry: %d FAILED\n" : "atv_geometry: all passed\n", fails);
    return fails ? 1 : 0;
}
