// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A picture larger than the GPU takes (the 8192-wide waterfall on the Raspberry Pi 4/5's 4096-texel V3D, #284) is kept at the largest
// size it takes: each update of the picture is shrunk onto the smaller texture. Of the pixels that fall on one texel the brightest is kept,
// so a narrow signal in a waterfall does not vanish. Header-only, no GL: the OpenGL back end uses it and tests/test_tex_shrink.cpp checks it.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace texshrink {

// the side of the texture for a picture side n, with the GPU's limit
inline int side(int n, int maxTex) { return n < 1 ? 1 : n > maxTex ? maxTex : n; }

// The texels X0..X1, Y0..Y1 (end exclusive) an update of the picture's x..x+w, y..y+h covers, picture lw x lh on texture pw x ph.
inline void rect(int x, int y, int w, int h, int lw, int lh, int pw, int ph, int& X0, int& X1, int& Y0, int& Y1) {
    X0 = (int)((long long)x * pw / lw); X1 = std::min(pw, (int)(((long long)(x + w) * pw + lw - 1) / lw));
    Y0 = (int)((long long)y * ph / lh); Y1 = std::min(ph, (int)(((long long)(y + h) * ph + lh - 1) / lh));
}

// The update rgba (w x h at x, y of the picture) shrunk onto the texels rect() gives, row after row into out. False: nothing to upload.
inline bool shrink(const uint32_t* rgba, int x, int y, int w, int h, int lw, int lh, int pw, int ph,
                   std::vector<uint32_t>& out, int& X0, int& X1, int& Y0, int& Y1) {
    rect(x, y, w, h, lw, lh, pw, ph, X0, X1, Y0, Y1);
    if (X1 <= X0 || Y1 <= Y0 || w <= 0 || h <= 0) return false;
    out.resize((size_t)(X1 - X0) * (Y1 - Y0));
    for (int Y = Y0; Y < Y1; Y++) {
        const int sy0 = std::min(y + h - 1, std::max(y, (int)((long long)Y * lh / ph)));
        const int sy1 = std::min(y + h, std::max(sy0 + 1, (int)((long long)(Y + 1) * lh / ph)));
        for (int X = X0; X < X1; X++) {
            const int sx0 = std::min(x + w - 1, std::max(x, (int)((long long)X * lw / pw)));
            const int sx1 = std::min(x + w, std::max(sx0 + 1, (int)((long long)(X + 1) * lw / pw)));
            uint32_t best = rgba[(size_t)(sy0 - y) * w + (sx0 - x)];
            int bestL = -1;
            for (int sy = sy0; sy < sy1; sy++)
                for (int sx = sx0; sx < sx1; sx++) {
                    const uint32_t p = rgba[(size_t)(sy - y) * w + (sx - x)];
                    const int l = (int)(p & 0xFF) + (int)((p >> 8) & 0xFF) + (int)((p >> 16) & 0xFF);
                    if (l > bestL) { bestL = l; best = p; }
                }
            out[(size_t)(Y - Y0) * (X1 - X0) + (X - X0)] = best;
        }
    }
    return true;
}

}  // namespace texshrink
