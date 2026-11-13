// The OpenGL versions OnAir tries, best first (pure table, no GLFW): a Raspberry Pi 5 / CM5 with Mesa offers only 3.1 (#283).
// Level 0 is what OnAir always asked for; each later level needs less of the driver. The desktop chain is the first kGlLevelCount entries;
// kGlesLevel (OpenGL ES 3.0 through EGL) is tried only on Linux, when desktop OpenGL comes back as a software renderer (llvmpipe) while a GPU
// render node is there: a Raspberry Pi 5's V3D is reached through EGL / GLES where the X server's GLX hands out llvmpipe (#470).
#pragma once
#include <cstddef>
#include <cstring>

namespace gfx {

struct GlLevel {
    int major, minor;
    bool core;          // request the core profile with forward compatibility (3.2+); false: no profile hint
    const char* glsl;   // the first line of every shader and the string for ImGui_ImplOpenGL3_Init
    bool es = false;    // OpenGL ES (through EGL): the shaders get default precisions after the version line
};

constexpr GlLevel kGlLevels[] = {
    {3, 2, true,  "#version 150"},
    {3, 1, false, "#version 140"},
    {3, 0, false, "#version 130"},
    {3, 0, false, "#version 300 es", true},
};
constexpr int kGlLevelAll = (int)(sizeof kGlLevels / sizeof kGlLevels[0]);
constexpr int kGlLevelCount = kGlLevelAll - 1;   // the desktop chain
constexpr int kGlesLevel = kGlLevelAll - 1;      // OpenGL ES 3.0 through EGL (Linux, #470)
constexpr const char* kGlesPrecision = "precision highp float;\nprecision highp int;\n";

inline const GlLevel& glLevel(int i) { return kGlLevels[i < 0 ? 0 : i >= kGlLevelAll ? kGlLevelAll - 1 : i]; }

// the shader source with the level's version line in front of the body (which has none); out is empty when it does not fit
inline void glShaderText(char* out, size_t cap, int level, const char* body) {
    if (cap == 0) return;
    const char* v = glLevel(level).glsl;
    const char* pr = glLevel(level).es ? kGlesPrecision : "";
    const size_t n = std::strlen(v), p = std::strlen(pr), m = std::strlen(body);
    if (n + 1 + p + m + 1 > cap) { out[0] = 0; return; }
    std::memcpy(out, v, n);
    out[n] = '\n';
    std::memcpy(out + n + 1, pr, p);
    std::memcpy(out + n + 1 + p, body, m + 1);
}

}
