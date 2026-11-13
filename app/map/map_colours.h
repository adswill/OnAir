// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Map colour schemes (sea/background, grid, coastline, label text, tile tint) and the contrast helpers that keep the overlays readable.
// No ImGui here: tests/app/test_map_colours.cpp runs the very table and functions the map draws with.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mapcol {
struct Scheme { uint32_t sea, grid, coast, label, tint; };   // 0xRRGGBB; tint multiplies the tile pictures (0xFFFFFF leaves them alone)
struct Preset { const char* name; Scheme s; };

constexpr int kDefault = 0;   // today's look, exactly
constexpr Preset kPresets[] = {
    {"Default",              {0x0E1012, 0x1E2226, 0x3A4A42, 0x969A9E, 0x78808C}},
    {"Dark",                 {0x000000, 0x16181A, 0x4A5A66, 0xB4B8BC, 0x606870}},
    {"Light",                {0xDCE6EE, 0xBCC8D2, 0x5A6A5E, 0x2A2E32, 0xFFFFFF}},
    {"Classic",              {0xAAC8E6, 0x8CAAC8, 0x3C6E3C, 0x1C2630, 0xFFFFFF}},
    {"Muted grey",           {0x2A2C2E, 0x38393B, 0x8A8E92, 0xC8CACC, 0x909498}},
    {"Satellite-ish dark",   {0x060A12, 0x10161E, 0x46603E, 0xAEB6BE, 0x8C9488}},
};
constexpr int kPresetCount = (int)(sizeof kPresets / sizeof kPresets[0]);
constexpr int kCustom = kPresetCount;   // the selection value for "Custom"
constexpr float kMinLabelRatio = 4.5f;  // WCAG AA, asked of the label colour on the sea colour

struct Rgb { float r, g, b; };
inline Rgb unpack(uint32_t c) { return {((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f}; }
inline uint32_t pack(Rgb c) {
    auto q = [](float x) { return (uint32_t)std::lround(std::min(1.f, std::max(0.f, x)) * 255.f); };
    return (q(c.r) << 16) | (q(c.g) << 8) | q(c.b);
}
inline float lin(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
inline float luminance(uint32_t c) { const Rgb x = unpack(c); return 0.2126f * lin(x.r) + 0.7152f * lin(x.g) + 0.0722f * lin(x.b); }
inline float ratio(uint32_t a, uint32_t b) { const float x = luminance(a), y = luminance(b); return (std::max(x, y) + 0.05f) / (std::min(x, y) + 0.05f); }
inline bool lightBg(uint32_t bg) { return luminance(bg) > 0.35f; }

// The colour the overlay text (labels, "me", the credit) is drawn in on this background: whichever of near-white / near-black holds the higher ratio.
inline uint32_t textOn(uint32_t bg) { return ratio(bg, 0xF0F0F0) >= ratio(bg, 0x101418) ? 0xF0F0F0 : 0x101418; }
// The backing for the floating panels (layer list, legends).
inline uint32_t panelOn(uint32_t bg) { return lightBg(bg) ? 0xF2F5F8 : 0x0F1114; }
// A marker/line colour made readable on bg: pushed towards black or white, hue kept, until it holds minRatio (as far as it goes).
inline uint32_t legibleOn(uint32_t c, uint32_t bg, float minRatio = 3.0f) {
    if (ratio(c, bg) >= minRatio) return c;
    const Rgb x = unpack(c); const bool toBlack = lightBg(bg);
    auto mix = [&](float t) { return toBlack ? Rgb{x.r * (1 - t), x.g * (1 - t), x.b * (1 - t)} : Rgb{x.r + (1 - x.r) * t, x.g + (1 - x.g) * t, x.b + (1 - x.b) * t}; };
    float lo = 0.f, hi = 1.f;
    for (int i = 0; i < 12; i++) {
        const float t = 0.5f * (lo + hi);
        if (ratio(pack(mix(t)), bg) >= minRatio) hi = t; else lo = t;
    }
    return pack(mix(hi));
}
}
