// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The pure parts of the Linux / software-OpenGL performance work (docs/PERF-LINUX.md): renderer detection, the low-power choice,
// the frame-gap floor, frame-time percentiles (app/gfx/render_caps.h) and the trail thinning and culling (app/adsb/trail_draw.h).
#include "../../app/gfx/render_caps.h"
#include "../../app/adsb/trail_draw.h"
#include <cmath>
#include <cstdio>
#include <vector>

static int fails = 0;
static void check(bool ok, const char* what) { if (!ok) { printf("FAIL: %s\n", what); fails++; } }

int main() {
    // renderer strings as Mesa, Windows and the GPU drivers report them
    check(rcaps::isSoftwareRenderer("llvmpipe (LLVM 19.1.1, 256 bits)"), "llvmpipe");
    check(rcaps::isSoftwareRenderer("softpipe"), "softpipe");
    check(rcaps::isSoftwareRenderer("Mesa X11 swrast"), "swrast");
    check(rcaps::isSoftwareRenderer("Software Rasterizer"), "classic swrast name");
    check(rcaps::isSoftwareRenderer("GDI Generic"), "Windows without a driver");
    check(rcaps::isSoftwareRenderer("LLVMPIPE (LLVM 15)"), "case");
    check(!rcaps::isSoftwareRenderer("Mesa Intel(R) UHD Graphics 620 (KBL GT2)"), "Intel");
    check(!rcaps::isSoftwareRenderer("AMD Radeon RX 6600 (radeonsi, navi23, LLVM 17.0.6, DRM 3.54)"), "radeonsi (mentions LLVM)");
    check(!rcaps::isSoftwareRenderer("NVIDIA GeForce GTX 1060/PCIe/SSE2"), "NVIDIA");
    check(!rcaps::isSoftwareRenderer("V3D 4.2"), "Raspberry Pi 4");
    check(!rcaps::isSoftwareRenderer("virgl (Apple M1)"), "virgl is a GPU");
    check(!rcaps::isSoftwareRenderer(nullptr) && !rcaps::isSoftwareRenderer(""), "none");

    // the choice: the environment / flag win, the setting otherwise, nothing chosen = off
    check(!rcaps::lowPowerWanted(rcaps::Ask, -1), "ask: off until answered");
    check(rcaps::lowPowerWanted(rcaps::On, -1) && !rcaps::lowPowerWanted(rcaps::Off, -1), "setting");
    check(rcaps::lowPowerWanted(rcaps::Off, 1) && !rcaps::lowPowerWanted(rcaps::On, 0), "forced");
    check(rcaps::showHint(true, rcaps::Ask, -1), "hint with software and no choice");
    check(!rcaps::showHint(false, rcaps::Ask, -1) && !rcaps::showHint(true, rcaps::Off, -1) && !rcaps::showHint(true, rcaps::Ask, 1), "no hint otherwise");

    // the frame gap: never faster than the pacing asks for while there is input; low-power never faster than before
    check(rcaps::minFrameGap(false, false, true, true) == 0, "macOS / Windows with a GPU: unchanged");
    check(std::fabs(rcaps::minFrameGap(true, false, true, true) - 1.0 / 60) < 1e-12, "Linux: 60 fps floor");
    for (int in = 0; in < 2; in++)
        for (int run = 0; run < 2; run++) {
            const double lp = rcaps::minFrameGap(true, true, in, run), full = rcaps::minFrameGap(true, false, in, run);
            check(lp >= full && lp >= 1.0 / 30 - 1e-12 && lp <= 0.25, "low power: slower, at most 4 fps idle");
        }
    check(rcaps::minFrameGap(true, true, false, true) == 0.1, "low power receiving: 10 fps");

    // percentiles (nearest rank)
    {
        std::vector<float> v;
        for (int i = 100; i >= 1; i--) v.push_back((float)i);
        check(rcaps::FrameStats::percentile(v, 0.5) == 51 || rcaps::FrameStats::percentile(v, 0.5) == 50, "p50");
        check(rcaps::FrameStats::percentile(v, 0.99) == 99, "p99");
        check(rcaps::FrameStats::percentile(v, 1.0) == 100 && rcaps::FrameStats::percentile(v, 0) == 1, "ends");
        check(rcaps::FrameStats::percentile({}, 0.5) == 0, "empty");
        rcaps::FrameStats fs; fs.add(0.016, 0.004); fs.add(0.033, 0.010);
        check(fs.size() == 2, "stats size");
    }

    // trails: thinning keeps the ends and every point at least minPx from the last one drawn
    {
        const trails::Rect view{0, 0, 100, 100};
        std::vector<trails::Pt> p;
        for (int i = 0; i < 80; i++) p.push_back({50 + i * 0.1f, 50});   // 80 points over 7.9 px: a zoomed-out trail
        std::vector<std::pair<size_t, size_t>> seg;
        const size_t n = trails::forEachSegment(p.data(), p.size(), 1.5f, view, [&](size_t a, size_t b) { seg.push_back({a, b}); });
        check(n == seg.size() && n >= 5 && n <= 6, "80 sub-pixel points: about 7.9 / 1.5 segments");
        check(!seg.empty() && seg.front().first == 0 && seg.back().second == p.size() - 1, "first and newest point kept");
        bool chained = true;
        for (size_t i = 1; i < seg.size(); i++) chained = chained && seg[i].first == seg[i - 1].second;
        check(chained, "segments join up");
        for (const auto& s : seg) {
            if (s.second == p.size() - 1) continue;
            const float d = std::hypot(p[s.second].x - p[s.first].x, p[s.second].y - p[s.first].y);
            check(d >= 1.5f - 1e-4f, "no segment shorter than minPx but the last");
        }
        // with a 0 px minimum every segment is drawn (the full-detail result)
        seg.clear();
        check(trails::forEachSegment(p.data(), p.size(), 0.f, view, [&](size_t a, size_t b) { seg.push_back({a, b}); }) == p.size() - 1, "minPx 0: all");
    }
    {   // culling: off one side is dropped, crossing the view or crossing a corner region is kept
        const trails::Rect view{0, 0, 100, 100};
        const trails::Pt off[] = {{-50, 10}, {-20, 90}, {-5, 200}};   // all left of the view
        check(trails::forEachSegment(off, 3, 0.f, view, [](size_t, size_t) {}) == 0, "left of the view");
        const trails::Pt cross[] = {{-50, 50}, {150, 50}};             // through it
        check(trails::forEachSegment(cross, 2, 0.f, view, [](size_t, size_t) {}) == 1, "through the view");
        const trails::Pt diag[] = {{-10, 50}, {50, -10}};              // outside at both ends, on different sides: may cut the corner
        check(trails::forEachSegment(diag, 2, 0.f, view, [](size_t, size_t) {}) == 1, "corner kept");
        check(trails::forEachSegment(diag, 1, 0.f, view, [](size_t, size_t) {}) == 0, "one point: nothing");
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("render_caps: all passed\n");
    return 0;
}
