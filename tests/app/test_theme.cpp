// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Light and High contrast themes: the text colours of the palette (theme.h's pal:: and the style's text greys) keep a WCAG contrast
// ratio of at least 4.5:1 on that theme's panes once main.cpp has recoloured the frame (contrast.h), and the Light turn-over is exact.
#include "ui/theme.h"
#include "ui/contrast.h"
#include <cstdio>
#include <vector>

using contrast::Rgb;
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

static Rgb rgb(ImVec4 v) { return {v.x, v.y, v.z}; }

int main() {
    struct Role { const char* name; ImVec4 c; };
    pal::dev() = false;
    const ImVec4 headClassic = pal::heading();
    pal::dev() = true;
    const ImVec4 headNew = pal::heading();
    pal::dev() = false;
    const std::vector<Role> roles = {
        {"okGreen", pal::okGreen()}, {"warnAmber", pal::warnAmber()}, {"badRed", pal::badRed()}, {"accent", pal::accent()},
        {"heading (classic)", headClassic}, {"heading (new)", headNew}, {"grey", pal::grey()},
        {"classic Text", ImVec4(0.82f, 0.84f, 0.85f, 1)}, {"classic TextDisabled", ImVec4(0.45f, 0.47f, 0.49f, 1)}, {"plot text", ImVec4(0.62f, 0.64f, 0.66f, 1)},
        {"instrument text", ImVec4(0.86f, 0.87f, 0.88f, 1)}, {"instrument dim", ImVec4(0.60f, 0.61f, 0.62f, 1)},
        {"mono text", ImVec4(0.84f, 0.84f, 0.84f, 1)}, {"mono dim", ImVec4(0.50f, 0.50f, 0.50f, 1)},
        {"terminal text", ImVec4(0.82f, 0.81f, 0.78f, 1)}, {"terminal dim", ImVec4(0.46f, 0.45f, 0.42f, 1)},
        {"terminal accent", ImVec4(0.86f, 0.65f, 0.30f, 1)}, {"heart rate red", ImVec4(0.95f, 0.35f, 0.35f, 1)},
        {"error orange", ImVec4(0.95f, 0.5f, 0.3f, 1)}, {"scrambled amber", ImVec4(0.95f, 0.7f, 0.2f, 1)},
    };

    // the turn-over is its own inverse and keeps the hue's spread
    for (const Role& r : roles) {
        const Rgb c = rgb(r.c), t = contrast::invertLightness(contrast::invertLightness(c));
        CHECK(std::fabs(t.r - c.r) < 1e-4f && std::fabs(t.g - c.g) < 1e-4f && std::fabs(t.b - c.b) < 1e-4f, "%s: the turn-over is not an involution", r.name);
    }
    // the panes of the Light theme: the dark panes turned over, and the clear colour; kLightBgY must not be lighter than any of them
    for (ImVec4 p : {ImVec4(0.055f, 0.058f, 0.062f, 1), ImVec4(0.075f, 0.078f, 0.083f, 1), ImVec4(0.165f, 0.168f, 0.172f, 1), ImVec4(0.035f, 0.037f, 0.040f, 1)}) {
        const float y = contrast::luminance(contrast::invertLightness(rgb(p)));
        CHECK(y >= contrast::kLightBgY - 0.02f, "a Light pane (%.3f) is darker than the contrast target pane (%.3f)", y, contrast::kLightBgY);
    }
    CHECK(contrast::luminance({0.955f, 0.95f, 0.96f}) >= contrast::kLightBgY - 0.02f, "the Light clear colour");

    int unfixedLight = 0;
    for (const Role& r : roles) {
        const Rgb dark = rgb(r.c);
        // Light: turned over, then made legible; against the darkest of the Light panes (the dark theme's lightest pane turned over)
        const Rgb light = contrast::legibleOnLight(contrast::invertLightness(dark));
        const Rgb pane = contrast::invertLightness({0.165f, 0.168f, 0.172f});   // the instrument palette's pane
        const float rl = contrast::ratio(light, {0.955f, 0.95f, 0.96f}), rp = contrast::ratio(light, pane);
        CHECK(rl >= 4.5f, "Light: %s has %.2f:1 on the clear colour", r.name, rl);
        CHECK(rp >= 4.5f, "Light: %s has %.2f:1 on the instrument pane", r.name, rp);
        if (contrast::ratio(contrast::invertLightness(dark), {0.955f, 0.95f, 0.96f}) < 4.5f) unfixedLight++;
        // High contrast: lifted on black
        const Rgb hc = contrast::legibleOnDark(dark);
        const float rh = contrast::ratio(hc, {0, 0, 0});
        CHECK(rh >= 7.f, "High contrast: %s has %.2f:1 on black", r.name, rh);
        std::printf("  %-22s Light %.1f:1  High contrast %.1f:1\n", r.name, rl, rh);
    }
    CHECK(unfixedLight > 0, "no palette colour needed the Light fix: the check does nothing");   // amber and green fail without it
    // the High contrast style colours themselves
    CHECK(contrast::ratio({1, 1, 1}, {0, 0, 0}) >= 7.f && contrast::ratio({1.f, 0.9f, 0.f}, {0, 0, 0}) >= 7.f && contrast::ratio({0.78f, 0.78f, 0.78f}, {0, 0, 0}) >= 7.f, "High contrast text/accent/disabled on black");
    CHECK(contrast::ratio({1, 1, 1}, {0.05f, 0.05f, 0.55f}) >= 7.f, "High contrast: white on the active fill");
    CHECK(contrast::ratio({1, 1, 1}, {0.22f, 0.22f, 0.22f}) >= 7.f, "High contrast: white on the hovered fill");

    std::printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
