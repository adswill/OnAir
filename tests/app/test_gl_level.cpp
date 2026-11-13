// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The OpenGL fallback table (app/gfx/gl_level.h, #283): 3.2 core stays the first level, lower ones follow with matching GLSL versions.
#include "../../app/gfx/gl_level.h"
#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    using namespace gfx;
    CHECK(kGlLevelCount == 3, "three levels");
    CHECK(glLevel(0).major == 3 && glLevel(0).minor == 2 && glLevel(0).core && !std::strcmp(glLevel(0).glsl, "#version 150"), "level 0 is 3.2 core / GLSL 150, as before");
    CHECK(glLevel(1).major == 3 && glLevel(1).minor == 1 && !glLevel(1).core && !std::strcmp(glLevel(1).glsl, "#version 140"), "level 1 is 3.1 / GLSL 140");
    CHECK(glLevel(2).major == 3 && glLevel(2).minor == 0 && !glLevel(2).core && !std::strcmp(glLevel(2).glsl, "#version 130"), "level 2 is 3.0 / GLSL 130");
    for (int i = 1; i < kGlLevelCount; i++)
        CHECK(glLevel(i).major * 10 + glLevel(i).minor < glLevel(i - 1).major * 10 + glLevel(i - 1).minor, "levels go down");
    CHECK(&glLevel(-5) == &glLevel(0) && &glLevel(99) == &glLevel(kGlLevelCount - 1), "indices clamp");
    char buf[64];
    glShaderText(buf, sizeof buf, 1, "void main() {}");
    CHECK(!std::strcmp(buf, "#version 140\nvoid main() {}"), "version line first, then the body: '%s'", buf);
    glShaderText(buf, 20, 0, "void main() {}");
    CHECK(buf[0] == 0, "too small a buffer gives an empty string");
    std::printf(failures ? "FAILED\n" : "gl_level ok\n");
    return failures ? 1 : 0;
}
