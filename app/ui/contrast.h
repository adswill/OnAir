// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Colour maths of the Light and High contrast themes (WCAG 2.x contrast ratios). No ImGui here, so the unit test (tests/app/test_theme.cpp)
// runs the very functions main.cpp applies to the text the interface draws.
#pragma once
#include <algorithm>
#include <cmath>

namespace contrast {
struct Rgb { float r, g, b; };   // sRGB, 0..1

inline float lin(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
inline float luminance(Rgb c) { return 0.2126f * lin(c.r) + 0.7152f * lin(c.g) + 0.0722f * lin(c.b); }
inline float ratio(float y1, float y2) { return (std::max(y1, y2) + 0.05f) / (std::min(y1, y2) + 0.05f); }
inline float ratio(Rgb a, Rgb b) { return ratio(luminance(a), luminance(b)); }

// View > Light turns every colour's lightness over (L -> 1 - L, hue and saturation kept): per channel c + (1 - max - min). It is its own inverse.
inline Rgb invertLightness(Rgb c) {
    const float d = 1.f - std::max(c.r, std::max(c.g, c.b)) - std::min(c.r, std::min(c.g, c.b));
    auto cl = [](float x) { return std::min(1.f, std::max(0.f, x)); };
    return {cl(c.r + d), cl(c.g + d), cl(c.b + d)};
}

constexpr float kLightBgY = 0.65f;     // the luminance of the Light theme's darkest pane (the Instrument palette's grey pane turned over; the clear colour is 0.89)
constexpr float kLightMinRatio = 5.0f; // asked of the text colours on that pane (WCAG AA is 4.5:1; the margin covers the hovered rows)
constexpr float kHcBgY = 0.f;          // High contrast: black panes
constexpr float kHcMinRatio = 7.0f;    // WCAG AAA

// Light: text colours (after the inversion) that are too light for the white panes are darkened, hue kept, until the ratio holds.
// Near-white colours are left alone: that is light text on a dark chip the inversion made.
inline Rgb legibleOnLight(Rgb c) {
    const float y = luminance(c);
    if (y > 0.75f || ratio(y, kLightBgY) >= kLightMinRatio) return c;
    float lo = 0.f, hi = 1.f;   // the factor on the sRGB channels
    for (int i = 0; i < 12; i++) {
        const float k = 0.5f * (lo + hi);
        if (ratio(luminance({c.r * k, c.g * k, c.b * k}), kLightBgY) >= kLightMinRatio) lo = k; else hi = k;
    }
    return {c.r * lo, c.g * lo, c.b * lo};
}

// High contrast: dim text (dark greys, muted hues) is lifted towards white until it holds 7:1 on black, hue kept as far as it goes.
// Near-black colours are left alone: that is dark text on a bright chip.
inline Rgb legibleOnDark(Rgb c) {
    const float y = luminance(c);
    if (y < 0.02f || ratio(y, kHcBgY) >= kHcMinRatio) return c;
    float lo = 0.f, hi = 1.f;   // the blend towards white
    for (int i = 0; i < 12; i++) {
        const float t = 0.5f * (lo + hi);
        if (ratio(luminance({c.r + (1 - c.r) * t, c.g + (1 - c.g) * t, c.b + (1 - c.b) * t}), kHcBgY) >= kHcMinRatio) hi = t; else lo = t;
    }
    return {c.r + (1 - c.r) * hi, c.g + (1 - c.g) * hi, c.b + (1 - c.b) * hi};
}
}
