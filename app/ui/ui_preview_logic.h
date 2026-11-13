// The hover preview of the appearance choices (docs/UI-PREVIEW-4.0.md): hovering a palette, light / high contrast, layout, Classic or
// Unified, font look, text size, touch, Easy mode, waterfall colour map, trace colour or tuner style shows a small live picture of the
// interface with that choice. This header is the pure half: the settings in use (Look), the hovered option applied to a copy of them
// (with) and the description the miniature is drawn from (describe): final on-screen colours, spacing, font size and layout. Nothing
// is changed until the user picks. No ImGui here: tests/app/test_ui_preview.cpp checks it; app/ui/ui_preview.cpp draws it.
#pragma once
#include "palette_data.h"
#include "contrast.h"
#include "../gfx/colormap.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace uiprev {

// every appearance setting the preview knows, with the values App holds (app.h)
struct Look {
    bool newUi = true;        // App::newUi (false: View > Classic interface)
    int theme = 3;            // App::uiTheme: 0 terminal, 1 instrument, 2 mono, 3 Dev A, 4 Dev B, 5 Studio, 6 Mono Pro, 7 Daylight
    int mode = 0;             // 0 dark, 1 light (App::lightUi), 2 high contrast (App::highContrast)
    int variant = 0;          // App::uiVariant: 0 sidebar ... 7 panel
    bool unified = false;     // App::layoutUnified
    int typography = 0;       // the "uiTypography" preference: 0 current, 1 clean, 2 compact
    float textScale = 1.f;    // App::uiTextScale
    bool touch = false;       // App::touchUi
    float touchScale = 1.35f; // App::touchScale
    bool easy = false;        // Easy mode (kit/mode_panel.h)
    int wfMap = -1;           // App::wf.map; -1 follows the palette
    int tunerStyle = 0;       // App::tunerStyle: 0 classic band, 1 simple lines
    int traceColour = 0;      // App::traceColor (cmap::kTracePresets)
    bool accentOn = false;    // Appearance > Own accent colour
    float accent[3] = {0.45f, 0.72f, 0.82f};
};

// what is hovered
enum class Opt { Theme, Mode, Variant, Unified, Typography, TextScale, Touch, Easy, WfMap, TunerStyle, TraceColour, NewUi };

// the settings after picking value v of option o; everything else stays as it is
inline Look with(Look l, Opt o, double v) {
    const int i = (int)std::lround(v);
    switch (o) {
    case Opt::Theme: l.theme = std::max(0, std::min(uipal::kNumPalettes - 1, i)); l.newUi = true; break;
    case Opt::Mode: l.mode = std::max(0, std::min(2, i)); break;
    case Opt::Variant: l.variant = std::max(0, std::min(7, i)); l.newUi = true; break;
    case Opt::Unified: l.unified = i != 0; break;
    case Opt::Typography: l.typography = std::max(0, std::min(2, i)); break;
    case Opt::TextScale: l.textScale = std::max(0.8f, std::min(2.f, (float)v)); break;
    case Opt::Touch: l.touch = i != 0; break;
    case Opt::Easy: l.easy = i != 0; break;
    case Opt::WfMap: l.wfMap = std::max(-1, std::min((int)cmap::NumMaps - 1, i)); break;
    case Opt::TunerStyle: l.tunerStyle = std::max(0, std::min(1, i)); break;
    case Opt::TraceColour: l.traceColour = std::max(0, std::min(cmap::kNumTracePresets - 1, i)); break;
    case Opt::NewUi: l.newUi = i != 0; break;
    }
    return l;
}

// the layouts of the miniature: the interface variants, the classic interface, the touch interface
enum Layout { kClassic = -1, kSidebar = 0, kScope, kTiles, kFaceplate, kCinema, kMeters, kDock, kPanel, kTouch = 100 };

// what the miniature is drawn from. Colours are 0xAABBGGRR (ImGui's IM_COL32 order) as they appear on the screen, after the Light pass.
struct Desc {
    uint32_t bg, rail, edge, text, dim, bright, accent, button, frame, plotBg, tabSel, tabText, chassis, trace, tuneEdge, tuneCentre, ok, warn;
    int wfMap;                 // the colour map the waterfall shows (the palette's own when the setting follows it)
    float radius;              // control corner radius, px at scale 1
    float padX, padY, spaceY;  // frame padding and row spacing, px at scale 1
    float fontPx;              // text size, px at scale 1
    float scale;               // text size and touch scale on top of the display scale
    float border;              // outline width, px at scale 1
    bool sansFont;             // Inter (Clean, Compact) instead of the system monospaced font
    int layout;                // Layout
    bool unified, easy, leds, tunerSimple;
    bool light, highContrast;  // the look is light / black-white-yellow
    bool plotOnly;             // only the spectrum and waterfall (colour map, trace colour, tuner style)
    bool strip;                // a gradient strip of the colour map above them
};

inline uint32_t pack(float r, float g, float b, float a = 1.f) {
    auto q = [](float x) { return (uint32_t)std::lround(std::min(1.f, std::max(0.f, x)) * 255.f); };
    return q(r) | (q(g) << 8) | (q(b) << 16) | (q(a) << 24);
}
inline uint32_t pack(const uipal::C& c, float a = 1.f) { return pack(c.r, c.g, c.b, a); }
inline contrast::Rgb rgbOf(uint32_t c) { return {(c & 255) / 255.f, ((c >> 8) & 255) / 255.f, ((c >> 16) & 255) / 255.f}; }
inline uint32_t withAlpha(uint32_t c, float a) { return (c & 0x00FFFFFFu) | ((uint32_t)std::lround(std::min(1.f, std::max(0.f, a)) * 255.f) << 24); }
inline uipal::C mix(const uipal::C& x, const uipal::C& y, float t) { return {x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t}; }

// main.cpp turns the frame's lightness over (light_theme.cpp) when Light is on or the Daylight palette is used, and not in High contrast
inline bool frameTurned(const Look& l) { return l.mode != 2 && (l.mode == 1 || (l.newUi && l.theme == 7)); }

// the integer lightness turn-over of light_theme.cpp (c' = c + 255 - max - min per channel). It is exactly its own inverse, so a colour
// drawn as forFrame(c) in a frame that will be turned over appears as c: the miniature shows the hovered look, not the look in use.
inline uint32_t forFrame(uint32_t c) {
    const int r = (int)(c & 255), g = (int)((c >> 8) & 255), b = (int)((c >> 16) & 255);
    const int d = 255 - std::max(r, std::max(g, b)) - std::min(r, std::min(g, b));
    auto cl = [](int x) { return (uint32_t)std::max(0, std::min(255, x)); };
    return (c & 0xFF000000u) | cl(r + d) | (cl(g + d) << 8) | (cl(b + d) << 16);
}

// the waterfall colour map that shows (ui2_theme.cpp applyUiThemeBase, Waterfall::effMap)
inline int effectiveMap(int map, bool newUi, int theme) {
    if (newUi && theme >= 5 && map == cmap::NavyHeat) map = -1;   // the 3.5 designs bring their own map unless one was picked
    else if ((!newUi || theme < 5) && map == -1) map = cmap::NavyHeat;
    if (map >= 0 && map < cmap::NumMaps) return map;
    const int m = !newUi ? 0 : theme == 2 ? 1 : theme == 5 ? 3 : theme == 6 ? 4 : theme == 7 ? 5 : 2;   // pal::wfMode()
    return m == 5 ? cmap::Paper : m == 4 ? cmap::Amber : m == 3 ? cmap::Magma : m == 2 ? cmap::Turbo : m == 1 ? cmap::Grey : cmap::Classic;
}

inline Desc describe(const Look& l) {
    Desc d{};
    const bool hc = l.mode == 2, turned = frameTurned(l);
    // the source colours, as the style holds them before the frame pass
    uipal::Palette p;
    uipal::C tab{0.15f, 0.38f, 0.66f};   // pal::tabColours: the selected tab is a calm blue in every palette
    if (l.newUi) {
        p = uipal::palette(l.theme);
        const uipal::Metrics m = uipal::metrics(l.theme);
        d.radius = m.frameRounding; d.padX = m.padX; d.padY = m.padY; d.spaceY = m.spaceY;
        if (!hc && l.theme == 5) tab = mix(p.bg, p.accent, 0.78f);
        else if (!hc && l.theme == 6) tab = mix(p.bg, p.accent, 0.10f);
        else if (!hc && l.theme == 7) tab = p.rail;
    } else {   // the classic interface (theme.h applyTheme, then the font look's spacing of typography.cpp)
        p.bg = {0.055f, 0.058f, 0.062f}; p.rail = {0.14f, 0.14f, 0.14f}; p.edge = {0.20f, 0.21f, 0.22f};
        p.text = {0.82f, 0.84f, 0.85f}; p.dim = {0.45f, 0.47f, 0.49f}; p.bright = {1, 1, 1}; p.accent = {0.45f, 0.72f, 0.82f};
        p.frame = {0.035f, 0.037f, 0.040f}; p.button = {0.10f, 0.105f, 0.11f}; p.plotBg = {0.02f, 0.021f, 0.023f};
        d.radius = 3; d.padX = 6; d.padY = 2; d.spaceY = 3;
        if (l.typography != 0) {
            const bool c = l.typography == 2;
            d.radius = 4; d.padX = c ? 6.f : 8.f; d.padY = c ? 3.f : 4.f; d.spaceY = c ? 4.f : 6.f;
            p.text = {0.90f, 0.91f, 0.92f}; p.dim = {0.56f, 0.59f, 0.62f};
        }
    }
    if (l.accentOn && !hc) p.accent = {l.accent[0], l.accent[1], l.accent[2]};
    uipal::C chassis = l.variant == 7 ? uipal::C{0.735f, 0.728f, 0.706f} : uipal::C{0.705f, 0.712f, 0.720f};   // kPanelChassis, kChassis
    if (hc) {   // ui2_theme.cpp applyHighContrast: black panes, white text and rules, yellow for what is selected
        const uipal::C black{0, 0, 0}, white{1, 1, 1}, grey{0.07f, 0.07f, 0.07f};
        p.bg = black; p.rail = black; p.edge = white; p.text = white; p.dim = {0.78f, 0.78f, 0.78f}; p.bright = white;
        p.accent = {1.f, 0.90f, 0.f}; p.frame = grey; p.button = grey; p.plotBg = black;
        chassis = black;
    }
    const float* tr = cmap::traceRgb(l.traceColour, false);
    uipal::C trace{tr[0], tr[1], tr[2]}, tuneEdge{135 / 255.f, 138 / 255.f, 142 / 255.f}, tuneCentre{232 / 255.f, 48 / 255.f, 44 / 255.f};
    // what the screen shows: the Light pass turns every shape over and keeps text legible on the white panes
    auto shape = [&](const uipal::C& c) {
        if (!turned) return pack(c);
        const contrast::Rgb o = contrast::invertLightness({c.r, c.g, c.b});
        return pack(o.r, o.g, o.b);
    };
    auto glyph = [&](const uipal::C& c) {
        if (hc) { const contrast::Rgb o = contrast::legibleOnDark({c.r, c.g, c.b}); return pack(o.r, o.g, o.b); }
        if (!turned) return pack(c);
        const contrast::Rgb o = contrast::legibleOnLight(contrast::invertLightness({c.r, c.g, c.b}));
        return pack(o.r, o.g, o.b);
    };
    d.bg = shape(p.bg); d.rail = shape(p.rail); d.edge = shape(p.edge); d.text = glyph(p.text); d.dim = glyph(p.dim); d.bright = glyph(p.bright);
    d.accent = shape(p.accent); d.button = shape(p.button); d.frame = shape(p.frame); d.plotBg = shape(p.plotBg); d.tabSel = shape(tab);
    d.chassis = shape(chassis); d.trace = shape(trace); d.tuneEdge = shape(tuneEdge); d.tuneCentre = shape(tuneCentre);
    d.tabText = glyph(uipal::C{1, 1, 1});   // the selected tab's white label
    d.ok = shape(uipal::C{0.40f, 0.76f, 0.52f}); d.warn = shape(uipal::C{0.90f, 0.70f, 0.28f});   // pal::okGreen, pal::warnAmber
    d.wfMap = effectiveMap(l.wfMap, l.newUi, l.theme);   // the waterfall is a picture: the Light pass leaves it as it is
    d.fontPx = l.typography == 1 ? 14.f : 13.f;          // typography.cpp baseSize()
    d.sansFont = l.typography != 0;
    d.scale = std::max(0.8f, std::min(2.f, l.textScale)) * (l.touch ? std::max(1.f, std::min(2.f, l.touchScale)) : 1.f);
    d.border = hc ? 2.f : 1.f;
    if (l.touch) {   // touch::applyMetrics: 48 px targets, wider spacing, rounder corners
        d.padX = std::max(d.padX, 12.f); d.padY = std::max(d.padY, std::max(0.f, (48.f / d.scale - d.fontPx) * 0.5f));
        d.spaceY = std::max(d.spaceY, 8.f); d.radius = std::max(d.radius, 6.f);
    }
    d.layout = l.touch ? kTouch : !l.newUi ? kClassic : l.variant;
    d.unified = l.unified;
    d.easy = l.easy;
    d.leds = l.newUi && l.variant == 7 && !l.touch;
    d.tunerSimple = l.tunerStyle == 1;
    d.light = turned;
    d.highContrast = hc;
    d.plotOnly = false;
    d.strip = false;
    return d;
}

// the description for hovering value v of option o while `cur` is in use
inline Desc previewFor(const Look& cur, Opt o, double v) {
    Desc d = describe(with(cur, o, v));
    d.plotOnly = o == Opt::WfMap || o == Opt::TraceColour || o == Opt::TunerStyle;
    d.strip = o == Opt::WfMap;
    return d;
}

// the miniature's size at scale 1 (multiplied by the display scale gUi), and the pixels per real pixel inside it: the real interface
// shrunk to a half, at the hovered look's text size and touch scale (`now`: the look in use, whose scale gUi already holds)
constexpr float kShrink = 0.5f;
inline float previewW(const Desc&) { return 260.f; }
inline float previewH(const Desc& d) { return d.plotOnly ? (d.strip ? 128.f : 112.f) : 160.f; }
inline float unitFor(const Desc& now, const Desc& d, float gUi) { return kShrink * gUi / std::max(0.1f, now.scale) * d.scale; }

// the pictures of the dev page `dect2 --previewgallery` and of the render test: one per choice the menus offer
struct Sample { const char* name; Opt o; double v; };
inline const Sample kGallery[] = {
    {"Terminal", Opt::Theme, 0}, {"Instrument", Opt::Theme, 1}, {"Mono", Opt::Theme, 2}, {"Dev A", Opt::Theme, 3},
    {"Dev B", Opt::Theme, 4}, {"Studio", Opt::Theme, 5}, {"Mono Pro", Opt::Theme, 6}, {"Daylight", Opt::Theme, 7},
    {"Dark", Opt::Mode, 0}, {"Light", Opt::Mode, 1}, {"High contrast", Opt::Mode, 2}, {"Classic interface", Opt::NewUi, 0},
    {"Sidebar", Opt::Variant, 0}, {"Scope", Opt::Variant, 1}, {"Tiles", Opt::Variant, 2}, {"Faceplate", Opt::Variant, 3},
    {"Scope: Cinema", Opt::Variant, 4}, {"Scope: Meters", Opt::Variant, 5}, {"Scope: Dock", Opt::Variant, 6}, {"Panel", Opt::Variant, 7},
    {"Layout Classic", Opt::Unified, 0}, {"Layout Unified", Opt::Unified, 1}, {"Font Current", Opt::Typography, 0}, {"Font Clean", Opt::Typography, 1},
    {"Font Compact", Opt::Typography, 2}, {"Text 80 %", Opt::TextScale, 0.8}, {"Text 150 %", Opt::TextScale, 1.5}, {"Text 200 %", Opt::TextScale, 2.0},
    {"Touch interface", Opt::Touch, 1}, {"Easy mode", Opt::Easy, 1}, {"Tuner Classic", Opt::TunerStyle, 0}, {"Tuner Simple", Opt::TunerStyle, 1},
    {"Trace Amber", Opt::TraceColour, 1}, {"Map Navy heat", Opt::WfMap, cmap::NavyHeat}, {"Map Viridis", Opt::WfMap, cmap::Viridis}, {"Map Paper", Opt::WfMap, cmap::Paper},
};
constexpr int kGalleryCount = (int)(sizeof kGallery / sizeof *kGallery);

}   // namespace uiprev
