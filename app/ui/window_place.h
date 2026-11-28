// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Where a panel that lives in an OS window of its own (the Memory window on a second screen, issue #664) opens: the place it had when it
// was closed, kept only while its title bar is still reachable on some monitor, otherwise the middle of the first (primary) one.
// Pure logic, no GLFW: the monitors are passed in as work-area rectangles.
#pragma once
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace winplace {

struct Rect { int x = 0, y = 0, w = 0, h = 0; };

struct Place {
    bool out = false;      // the panel is in a window of its own (false: a window inside the main one)
    bool placed = false;   // x and y were saved (not on Wayland, where the compositor places windows)
    Rect r;                // x, y, w, h
};

constexpr int kMinW = 240, kMinH = 160, kMaxSize = 8192;

inline Rect clampSize(Rect r, int defW, int defH) {
    if (r.w <= 0) r.w = defW;
    if (r.h <= 0) r.h = defH;
    r.w = std::max(kMinW, std::min(r.w, kMaxSize));
    r.h = std::max(kMinH, std::min(r.h, kMaxSize));
    return r;
}

// At least a good part of the window's top (its title bar) must lie in some monitor's work area.
inline bool reachable(const Rect& r, const std::vector<Rect>& mons) {
    for (const Rect& m : mons) {
        const int ox = std::min(r.x + r.w, m.x + m.w) - std::max(r.x, m.x);
        if (ox >= std::min(r.w, 100) && r.y >= m.y && r.y < m.y + m.h - 40) return true;
    }
    return false;
}

// The rectangle to open at. A saved place that is still reachable is kept (the size too); otherwise the window is centred on the first
// monitor and shrunk to fit it. With no monitor list the saved place is used as it is.
inline Rect restore(const Place& p, const std::vector<Rect>& mons, int defW, int defH) {
    Rect r = clampSize(p.r, defW, defH);
    if (mons.empty()) return r;
    int maxW = 0, maxH = 0;   // a saved size never exceeds the biggest screen
    for (const Rect& m : mons) { maxW = std::max(maxW, m.w); maxH = std::max(maxH, m.h); }
    r.w = std::min(r.w, maxW); r.h = std::min(r.h, maxH);
    if (p.placed && reachable(r, mons)) return r;
    const Rect& m = mons.front();
    r.w = std::min(r.w, m.w); r.h = std::min(r.h, m.h);
    r.x = m.x + (m.w - r.w) / 2; r.y = m.y + (m.h - r.h) / 2;
    return r;
}

// Saved as one text line "out placed x y w h" (a preference string); a bad line gives the defaults (docked).
inline std::string write(const Place& p) {
    char b[96];
    std::snprintf(b, sizeof b, "%d %d %d %d %d %d", p.out ? 1 : 0, p.placed ? 1 : 0, p.r.x, p.r.y, p.r.w, p.r.h);
    return b;
}
inline Place parse(const std::string& s) {
    Place p;
    int o = 0, pl = 0, x = 0, y = 0, w = 0, h = 0;
    char extra = 0;
    if (std::sscanf(s.c_str(), "%d %d %d %d %d %d %c", &o, &pl, &x, &y, &w, &h, &extra) != 6) return p;
    p.out = o != 0; p.placed = pl != 0; p.r = Rect{x, y, w, h};
    return p;
}

}  // namespace winplace
