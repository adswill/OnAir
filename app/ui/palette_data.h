// The ten base colours of each palette of the new interface (View > palettes, Settings > Appearance > Colour palette), as plain data:
// ui2_theme.cpp's loadTheme() reads them from here, and so does the hover preview (ui_preview_logic.h), so the preview cannot drift from
// the real look. No ImGui here: tests/app/test_ui_preview.cpp checks it.
#pragma once
#include <algorithm>

namespace uipal {

struct C { float r = 0, g = 0, b = 0; };   // sRGB 0..1
struct Palette { C bg, rail, edge, text, dim, bright, accent, frame, button, plotBg; };

constexpr int kNumPalettes = 8;   // 0 terminal, 1 instrument, 2 mono, 3 Dev A, 4 Dev B, 5 Studio, 6 Mono Pro, 7 Daylight

inline C c8(int r, int g, int b) { return C{r / 255.f, g / 255.f, b / 255.f}; }
// Daylight is written in light colours; this turns one into the dark source colour the Light pass of main.cpp turns back (L -> 1 - L)
inline C lightSource(int r, int g, int b) {
    const int m = std::max(r, std::max(g, b)), lo = std::min(r, std::min(g, b));
    return c8(r + 255 - m - lo, g + 255 - m - lo, b + 255 - m - lo);
}

inline Palette palette(int n) {
    Palette p;
    if (n == 1) {
        p.bg = {0.165f, 0.168f, 0.172f}; p.rail = {0.135f, 0.138f, 0.142f}; p.edge = {0.33f, 0.335f, 0.34f};
        p.text = {0.86f, 0.87f, 0.88f}; p.dim = {0.60f, 0.61f, 0.62f}; p.bright = {1, 1, 1}; p.accent = {0.90f, 0.91f, 0.92f};
        p.frame = {0.095f, 0.098f, 0.102f}; p.button = {0.235f, 0.240f, 0.247f}; p.plotBg = {0.015f, 0.016f, 0.018f};
    } else if (n == 2) {
        p.bg = {0.075f, 0.075f, 0.075f}; p.rail = {0.055f, 0.055f, 0.055f}; p.edge = {0.22f, 0.22f, 0.22f};
        p.text = {0.84f, 0.84f, 0.84f}; p.dim = {0.50f, 0.50f, 0.50f}; p.bright = {1, 1, 1}; p.accent = {0.92f, 0.92f, 0.92f};
        p.frame = {0.045f, 0.045f, 0.045f}; p.button = {0.115f, 0.115f, 0.115f}; p.plotBg = {0.03f, 0.03f, 0.03f};
    } else if (n == 3) {   // Dev A (graphite, after a dark code editor)
        p.bg = c8(30, 30, 30); p.rail = c8(37, 37, 38); p.edge = c8(60, 60, 60);
        p.text = c8(204, 204, 204); p.dim = c8(133, 133, 133); p.bright = c8(240, 240, 240); p.accent = c8(59, 130, 196);
        p.frame = c8(24, 24, 24); p.button = c8(51, 51, 52); p.plotBg = c8(18, 18, 18);
    } else if (n == 4) {   // Dev B (terminal)
        p.bg = c8(12, 12, 12); p.rail = c8(26, 26, 26); p.edge = c8(51, 51, 51);
        p.text = c8(200, 200, 200); p.dim = c8(120, 120, 120); p.bright = c8(235, 235, 235); p.accent = c8(127, 217, 98);
        p.frame = c8(8, 8, 8); p.button = c8(34, 34, 34); p.plotBg = c8(6, 6, 6);
    } else if (n == 5) {   // Studio: soft slate-blue, one vivid violet accent
        p.bg = c8(26, 30, 42); p.rail = c8(33, 38, 54); p.edge = c8(54, 62, 86);
        p.text = c8(216, 222, 238); p.dim = c8(140, 150, 176); p.bright = c8(246, 248, 255); p.accent = c8(150, 112, 255);
        p.frame = c8(20, 24, 35); p.button = c8(45, 52, 74); p.plotBg = c8(15, 18, 27);
    } else if (n == 6) {   // Mono Pro: OLED black, amber
        p.bg = c8(3, 3, 3); p.rail = c8(0, 0, 0); p.edge = c8(52, 48, 40);
        p.text = c8(208, 204, 194); p.dim = c8(128, 120, 102); p.bright = c8(255, 240, 210); p.accent = c8(255, 176, 0);
        p.frame = c8(0, 0, 0); p.button = c8(16, 15, 12); p.plotBg = c8(0, 0, 0);
    } else if (n == 7) {   // Daylight (source colours; the Light pass turns them into the light look)
        p.bg = lightSource(243, 245, 250); p.rail = lightSource(255, 255, 255); p.edge = lightSource(196, 205, 222);
        p.text = lightSource(24, 31, 46); p.dim = lightSource(88, 99, 120); p.bright = lightSource(0, 0, 0); p.accent = lightSource(18, 92, 214);
        p.frame = lightSource(255, 255, 255); p.button = lightSource(228, 233, 243); p.plotBg = lightSource(252, 253, 255);
    } else {   // 0 terminal (and any unknown number)
        p.bg = {0.045f, 0.045f, 0.044f}; p.rail = {0.037f, 0.037f, 0.036f}; p.edge = {0.175f, 0.172f, 0.165f};
        p.text = {0.82f, 0.81f, 0.78f}; p.dim = {0.46f, 0.45f, 0.42f}; p.bright = {0.96f, 0.95f, 0.92f}; p.accent = {0.86f, 0.65f, 0.30f};
        p.frame = {0.030f, 0.030f, 0.030f}; p.button = {0.045f, 0.045f, 0.044f}; p.plotBg = {0.030f, 0.030f, 0.030f};
    }
    return p;
}

// corner radius of the controls (style FrameRounding at scale 1) and the spacing of each palette (ui2_theme.cpp applyUiThemeBase)
struct Metrics { float frameRounding, padX, padY, spaceX, spaceY; };
inline Metrics metrics(int n) {
    if (n == 5) return {7, 9, 4.5f, 9, 6};
    if (n == 6) return {0, 4, 1, 5, 2};
    if (n == 7) return {5, 8, 4, 8, 5};
    if (n == 3 || n == 4) return {0, 5, 2, 6, 3};
    return {1, 5, 1.5f, 6, 2.5f};
}

}   // namespace uipal
