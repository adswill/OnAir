// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pure parts of a workspace record (no GLFW, no App): the field escaping, the window fields (size, position, maximized) and the clamp
// of a saved window rectangle into the work areas of the connected monitors. Tested by tests/app/test_workspace_window.cpp.
#pragma once
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace wsrec {

struct Rect { int x = 0, y = 0, w = 0, h = 0; };

// The main window as saved: size and position in screen coordinates. have = false for a record saved before these fields (keep the window).
struct WindowState {
    bool have = false;
    int w = 0, h = 0, x = 0, y = 0;
    bool hasPos = false;    // the position was readable when saved (not on Wayland)
    bool maximized = false;
};

inline std::string escapeField(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '|') o += "\\p";
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o;
}

inline std::vector<std::string> splitRecord(const std::string& line) {
    std::vector<std::string> f(1);
    for (size_t i = 0; i < line.size(); i++) {
        const char c = line[i];
        if (c == '|') { f.emplace_back(); continue; }
        if (c == '\\' && i + 1 < line.size()) {
            const char n = line[++i];
            f.back() += n == 'p' ? '|' : n == 'n' ? '\n' : n;
            continue;
        }
        f.back() += c;
    }
    return f;
}

// trailing fields: w|h|x|y|hasPos|maximized (empty string when the window was not captured)
inline std::string encodeWindow(const WindowState& s) {
    if (!s.have) return std::string();
    return std::to_string(s.w) + "|" + std::to_string(s.h) + "|" + std::to_string(s.x) + "|" + std::to_string(s.y) + "|"
         + (s.hasPos ? "1" : "0") + "|" + (s.maximized ? "1" : "0");
}

// f[first..]: missing, empty or nonsense fields leave have = false
inline WindowState decodeWindow(const std::vector<std::string>& f, size_t first) {
    WindowState s;
    if (f.size() < first + 6 || f[first].empty()) return s;
    s.w = atoi(f[first].c_str()); s.h = atoi(f[first + 1].c_str());
    s.x = atoi(f[first + 2].c_str()); s.y = atoi(f[first + 3].c_str());
    s.hasPos = f[first + 4] == "1"; s.maximized = f[first + 5] == "1";
    s.have = s.w >= 200 && s.h >= 200;
    return s;
}

// The rectangle to give the window: the work area with the most overlap (the first when none overlaps, e.g. a monitor that is gone),
// the size cut to fit it, the position moved inside it. No work areas: the rectangle as it is.
inline Rect clampToWorkAreas(Rect r, const std::vector<Rect>& areas) {
    if (areas.empty()) return r;
    long best = -1; size_t bi = 0;
    for (size_t i = 0; i < areas.size(); i++) {
        const Rect& a = areas[i];
        const long ox = std::max(0, std::min(r.x + r.w, a.x + a.w) - std::max(r.x, a.x));
        const long oy = std::max(0, std::min(r.y + r.h, a.y + a.h) - std::max(r.y, a.y));
        if (ox * oy > best) { best = ox * oy; bi = i; }
    }
    const Rect& a = areas[bi];
    r.w = std::max(1, std::min(r.w, a.w)); r.h = std::max(1, std::min(r.h, a.h));
    r.x = std::max(a.x, std::min(r.x, a.x + a.w - r.w));
    r.y = std::max(a.y, std::min(r.y, a.y + a.h - r.h));
    return r;
}

}  // namespace wsrec
