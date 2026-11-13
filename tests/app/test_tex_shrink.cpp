// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A picture larger than the GPU's texture limit (app/gfx/tex_shrink.h, #284: the 8192-wide waterfall on the Raspberry Pi's 4096 V3D)
// is shrunk onto the largest texture the GPU takes, keeping the brightest pixel of each group.
#include "../../app/gfx/tex_shrink.h"
#include <cstdio>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    using namespace texshrink;
    CHECK(side(8192, 4096) == 4096 && side(512, 4096) == 512 && side(0, 4096) == 1, "texture side clamps to the GPU limit");

    // one waterfall row, 8192 wide onto 4096: a single bright bin anywhere stays visible
    const int lw = 8192, lh = 512, pw = 4096, ph = 512;
    std::vector<uint32_t> row(lw, 0xFF000000u), out;
    row[4097] = 0xFF00FFFFu;   // odd column: nearest sampling of even columns would lose it
    int X0, X1, Y0, Y1;
    CHECK(shrink(row.data(), 0, 7, lw, 1, lw, lh, pw, ph, out, X0, X1, Y0, Y1), "row shrinks");
    CHECK(X0 == 0 && X1 == pw && Y0 == 7 && Y1 == 8, "row 7 covers texels 0..4096 of texel row 7 (%d %d %d %d)", X0, X1, Y0, Y1);
    CHECK(out.size() == (size_t)pw && out[2048] == 0xFF00FFFFu, "the bright bin lands on texel 2048");
    int bright = 0;
    for (uint32_t p : out) bright += p != 0xFF000000u;
    CHECK(bright == 1, "only one texel is bright (%d)", bright);

    // a part of a row
    std::vector<uint32_t> part(100, 0xFF101010u);
    CHECK(shrink(part.data(), 1000, 3, 100, 1, lw, lh, pw, ph, out, X0, X1, Y0, Y1), "partial row");
    CHECK(X0 == 500 && X1 == 550 && Y0 == 3 && Y1 == 4, "partial row texels (%d %d)", X0, X1);
    CHECK(out.size() == 50 && out[0] == 0xFF101010u && out[49] == 0xFF101010u, "partial row values");

    // a tall picture (height over the limit too): 3 x 6000 onto 3 x 4096, every texel from inside the update
    std::vector<uint32_t> tall((size_t)3 * 6000);
    for (size_t i = 0; i < tall.size(); i++) tall[i] = 0xFF000000u | (uint32_t)(i / 3 % 200);
    CHECK(shrink(tall.data(), 0, 0, 3, 6000, 3, 6000, 3, 4096, out, X0, X1, Y0, Y1), "tall picture");
    CHECK(X1 - X0 == 3 && Y1 - Y0 == 4096 && out.size() == (size_t)3 * 4096, "tall size");

    // the same size: untouched
    std::vector<uint32_t> same(16, 0xFF123456u);
    CHECK(shrink(same.data(), 0, 0, 4, 4, 4, 4, 4, 4, out, X0, X1, Y0, Y1) && out == same, "same size copies");

    // nothing to upload
    CHECK(!shrink(same.data(), 0, 0, 0, 4, 4, 4, 4, 4, out, X0, X1, Y0, Y1), "empty update");
    std::printf(failures ? "FAILED\n" : "tex_shrink ok\n");
    return failures ? 1 : 0;
}
