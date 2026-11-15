// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Brightness and contrast of a picture at display time (issue #239): a 256-entry table applied to the RGB bytes of a copy of the frame
// that goes to the screen. The decoder and the recorded or saved frames are not touched. Pure functions.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace dect2 { namespace video {

struct DisplayAdjust {
    float brightness = 0.f;   // -1 .. 1: added after the contrast, as a fraction of full scale
    float contrast = 1.f;     // 0 .. 3: 1 leaves it, 0 is flat grey, 2 doubles the swing about mid grey

    bool identity() const { return std::fabs(brightness) < 1e-4f && std::fabs(contrast - 1.f) < 1e-4f; }
    void clamp() {
        brightness = std::isfinite(brightness) ? std::max(-1.f, std::min(1.f, brightness)) : 0.f;
        contrast = std::isfinite(contrast) ? std::max(0.f, std::min(3.f, contrast)) : 1.f;
    }
};

// out = (in - 128) * contrast + 128 + brightness * 255, clamped to 0..255
inline void makeLut(DisplayAdjust adj, uint8_t lut[256]) {
    adj.clamp();
    for (int i = 0; i < 256; i++) {
        const float v = ((float)i - 128.f) * adj.contrast + 128.f + adj.brightness * 255.f;
        lut[i] = (uint8_t)std::max(0.f, std::min(255.f, std::floor(v + 0.5f)));
    }
}

// RGBA pixels as bytes r, g, b, a in memory (the frames of the analog TV and FPV receivers): the colour bytes go through the table, alpha stays
inline void applyRgba(uint8_t* px, size_t pixels, const uint8_t lut[256]) {
    for (size_t i = 0; i < pixels; i++, px += 4) { px[0] = lut[px[0]]; px[1] = lut[px[1]]; px[2] = lut[px[2]]; }
}

inline void apply(uint32_t* px, size_t pixels, const DisplayAdjust& adj) {
    if (adj.identity()) return;
    uint8_t lut[256];
    makeLut(adj, lut);
    applyRgba(reinterpret_cast<uint8_t*>(px), pixels, lut);
}

}}
