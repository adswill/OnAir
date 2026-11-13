// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The OpenGL versions OnAir tries, best first (pure table, no GLFW): a Raspberry Pi 5 / CM5 with Mesa offers only 3.1 (#283).
// Level 0 is what OnAir always asked for; each later level needs less of the driver.
#pragma once
#include <cstddef>
#include <cstring>

namespace gfx {

struct GlLevel {
    int major, minor;
    bool core;          // request the core profile with forward compatibility (3.2+); false: no profile hint
    const char* glsl;   // the first line of every shader and the string for ImGui_ImplOpenGL3_Init
};

constexpr GlLevel kGlLevels[] = {
    {3, 2, true,  "#version 150"},
    {3, 1, false, "#version 140"},
    {3, 0, false, "#version 130"},
};
constexpr int kGlLevelCount = (int)(sizeof kGlLevels / sizeof kGlLevels[0]);

inline const GlLevel& glLevel(int i) { return kGlLevels[i < 0 ? 0 : i >= kGlLevelCount ? kGlLevelCount - 1 : i]; }

// the shader source with the level's version line in front of the body (which has none); out is empty when it does not fit
inline void glShaderText(char* out, size_t cap, int level, const char* body) {
    if (cap == 0) return;
    const char* v = glLevel(level).glsl;
    const size_t n = std::strlen(v), m = std::strlen(body);
    if (n + 1 + m + 1 > cap) { out[0] = 0; return; }
    std::memcpy(out, v, n);
    out[n] = '\n';
    std::memcpy(out + n + 1, body, m + 1);
}

}
