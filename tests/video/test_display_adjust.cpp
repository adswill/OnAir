// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/video/display_adjust.h"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace dect2::video;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    uint8_t lut[256];
    DisplayAdjust a;
    CHECK(a.identity(), "default is identity");
    makeLut(a, lut);
    for (int i = 0; i < 256; i++) CHECK(lut[i] == i, "identity lut at %d is %d", i, lut[i]);

    a.brightness = 0.2f; makeLut(a, lut);
    CHECK(lut[0] == 51 && lut[100] == 151 && lut[255] == 255 && lut[220] == 255, "brightness +0.2: %d %d %d", lut[0], lut[100], lut[255]);
    a.brightness = -0.5f; makeLut(a, lut);
    CHECK(lut[255] == 128 && lut[100] == 0 && lut[200] == 73, "brightness -0.5: %d %d %d", lut[255], lut[100], lut[200]);

    a = DisplayAdjust(); a.contrast = 2.f; makeLut(a, lut);
    CHECK(lut[128] == 128 && lut[64] == 0 && lut[192] == 255 && lut[100] == 72, "contrast 2: %d %d %d %d", lut[128], lut[64], lut[192], lut[100]);
    a.contrast = 0.f; makeLut(a, lut);
    CHECK(lut[0] == 128 && lut[255] == 128, "contrast 0 is flat grey");
    a.contrast = 0.5f; makeLut(a, lut);
    CHECK(lut[0] == 64 && lut[255] == 192 && lut[128] == 128, "contrast 0.5: %d %d", lut[0], lut[255]);

    // monotonic for any setting, ends clamped
    for (float b = -1; b <= 1; b += 0.25f)
        for (float c = 0; c <= 3; c += 0.5f) {
            DisplayAdjust d; d.brightness = b; d.contrast = c; makeLut(d, lut);
            for (int i = 1; i < 256; i++) CHECK(lut[i] >= lut[i - 1], "monotonic b=%.2f c=%.1f at %d", b, c, i);
        }

    // out of range and bad values are held to the limits
    a = DisplayAdjust(); a.brightness = 9; a.contrast = -4; a.clamp();
    CHECK(a.brightness == 1.f && a.contrast == 0.f, "clamp");
    a.brightness = NAN; a.contrast = INFINITY; a.clamp();
    CHECK(a.brightness == 0.f && a.contrast == 1.f, "non-finite values go back to neutral");

    // pixels: colour changes, alpha stays; identity leaves the array alone
    // little endian: the bytes in memory are r, g, b, a
    std::vector<uint32_t> px = {0xFF102030u, 0x80FFFFFFu, 0x00000000u};
    const std::vector<uint32_t> keep = px;
    apply(px.data(), px.size(), DisplayAdjust());
    CHECK(px == keep, "identity does not touch pixels");
    DisplayAdjust b; b.brightness = 0.1f;
    apply(px.data(), px.size(), b);
    const uint8_t* q = reinterpret_cast<const uint8_t*>(px.data());
    CHECK(q[0] == 0x30 + 26 && q[1] == 0x20 + 26 && q[2] == 0x10 + 26 && q[3] == 0xFF, "first pixel %d %d %d a=%d", q[0], q[1], q[2], q[3]);
    CHECK(q[4] == 255 && q[7] == 0x80, "second pixel saturates, alpha kept");
    CHECK(q[8] == 26 && q[11] == 0, "black lifted, alpha zero kept");

    std::printf(failures ? "display_adjust: %d FAILED\n" : "display_adjust: ok\n", failures);
    return failures ? 1 : 0;
}
