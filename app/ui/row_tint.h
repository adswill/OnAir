// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The tinted rows of the mode list (ui2_shell.cpp modeList): each category's heading and its mode rows sit on a background mixed from the
// pane and the category colour, the selected row on a stronger mix. No ImGui here, so tests/app/test_row_tint.cpp runs the same maths.
//
// The rows are drawn opaque (the mix is done here, not by alpha blending): the Light theme turns every vertex colour's lightness over after
// the frame is built (light_theme.cpp), and an opaque colour turns over exactly, so the check below sees the colours the user sees.
#pragma once
#include "contrast.h"

namespace rowtint {
using contrast::Rgb;

enum class Look { Dark, Light, HighContrast };   // the View > Theme choice (Daylight counts as Light: it is drawn through the same pass)

// how far the category colour is mixed into the pane (0 = the pane, 1 = the category colour)
constexpr float kHeadAlpha = 0.20f;    // the category's heading row
constexpr float kModeAlpha = 0.10f;    // its mode rows
constexpr float kHoverAlpha = 0.17f;   // a mode row under the pointer
constexpr float kSelAlpha = 0.34f;     // the selected mode
constexpr float kMinRatio = 4.5f;      // WCAG AA for the text on every tinted row, in every theme

inline Rgb mix(Rgb bg, Rgb c, float a) { return {bg.r + (c.r - bg.r) * a, bg.g + (c.g - bg.g) * a, bg.b + (c.b - bg.b) * a}; }

// a fill colour as the screen shows it
inline Rgb shownFill(Rgb c, Look l) { return l == Look::Light ? contrast::invertLightness(c) : c; }
// a text colour as the screen shows it (light_theme.cpp: turned over and darkened for Light, lifted for High contrast)
inline Rgb shownText(Rgb c, Look l) {
    if (l == Look::Light) return contrast::legibleOnLight(contrast::invertLightness(c));
    if (l == Look::HighContrast) return contrast::legibleOnDark(c);
    return c;
}

// The contrast of a text colour on a fill, both as shown
inline float shownRatio(Rgb fill, Rgb text, Look l) { return contrast::ratio(shownText(text, l), shownFill(fill, l)); }

// The text colour moved towards the brightest text (white in the source colours, black once Light turns it over) just far enough to hold
// minRatio on the fill; unchanged when it already does. The hierarchy of text, dim and bright is kept as far as the fill allows.
inline Rgb liftText(Rgb fill, Rgb text, Look l, float minRatio = kMinRatio) {
    if (shownRatio(fill, text, l) >= minRatio) return text;
    auto at = [&](float t) { return Rgb{text.r + (1 - text.r) * t, text.g + (1 - text.g) * t, text.b + (1 - text.b) * t}; };
    float lo = 0.f, hi = 1.f;
    for (int i = 0; i < 14; i++) {
        const float m = 0.5f * (lo + hi);
        if (shownRatio(fill, at(m), l) >= minRatio) hi = m; else lo = m;
    }
    return at(hi);
}

constexpr float kHeadroom = 7.0f;   // the brightest text must still hold this on a tint, so the lifted text keeps some room below it

// The strongest mix up to `want` at which the brightest text still holds kHeadroom on the row (a pale category colour on a dark pane is
// cut back; most are drawn as asked). 0 when even the bare pane cannot: the tint never makes a row worse.
inline float safeAlpha(Rgb bg, Rgb cat, float want, Look l) {
    const Rgb white{1, 1, 1};
    if (shownRatio(mix(bg, cat, want), white, l) >= kHeadroom) return want;
    float lo = 0.f, hi = want;
    for (int i = 0; i < 14; i++) {
        const float m = 0.5f * (lo + hi);
        if (shownRatio(mix(bg, cat, m), white, l) >= kHeadroom) lo = m; else hi = m;
    }
    return lo;
}

// the row's fill, ready to draw opaque
inline Rgb rowFill(Rgb bg, Rgb cat, float want, Look l) { return mix(bg, cat, safeAlpha(bg, cat, want, l)); }
}  // namespace rowtint
