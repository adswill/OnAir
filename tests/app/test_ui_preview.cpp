// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The hover preview of the appearance choices (app/ui/ui_preview_logic.h, app/ui/palette_data.h): every option value turns into the
// description the miniature is drawn from. Checked: the palette table holds the colours loadTheme() used before it read the table,
// every palette and layout gives a different picture, Light / High contrast / Daylight colours as the screen shows them (legible), the
// Light pass undone exactly for the miniature, the font look and text size metrics, the waterfall map a palette brings, the hovered
// option changes nothing else, and every combination of palette x theme x layout x font look x touch x Easy mode gives a sane picture.
#include "ui/ui_preview_logic.h"
#include <cstdio>
#include <set>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

using namespace uiprev;

static uint32_t rgb8(int r, int g, int b) { return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | 0xFF000000u; }
static float lum(uint32_t c) { return contrast::luminance(rgbOf(c)); }
static float ratio(uint32_t a, uint32_t b) { return contrast::ratio(rgbOf(a), rgbOf(b)); }
// the Light pass of app/light_theme.cpp on one shape colour, as it runs on the frame's vertices
static uint32_t framePass(uint32_t col) {
    const int r = (int)(col & 255), g = (int)((col >> 8) & 255), b = (int)((col >> 16) & 255);
    const int d = 255 - std::max(r, std::max(g, b)) - std::min(r, std::min(g, b));
    auto cl = [](int x) { return (uint32_t)std::max(0, std::min(255, x)); };
    return (col & 0xFF000000u) | cl(r + d) | (cl(g + d) << 8) | (cl(b + d) << 16);
}

static void paletteTable() {
    // spot values of the literals loadTheme() had before it read palette_data.h
    CHECK(pack(uipal::palette(3).accent) == rgb8(59, 130, 196), "Dev A accent");
    CHECK(pack(uipal::palette(4).accent) == rgb8(127, 217, 98), "Dev B accent");
    CHECK(pack(uipal::palette(5).accent) == rgb8(150, 112, 255), "Studio accent");
    CHECK(pack(uipal::palette(6).accent) == rgb8(255, 176, 0), "Mono Pro accent");
    CHECK(pack(uipal::palette(0).accent) == pack(0.86f, 0.65f, 0.30f), "terminal accent");
    CHECK(pack(uipal::palette(1).bg) == pack(0.165f, 0.168f, 0.172f), "instrument background");
    CHECK(pack(uipal::palette(2).text) == pack(0.84f, 0.84f, 0.84f), "mono text");
    CHECK(pack(uipal::palette(99).bg) == pack(uipal::palette(0).bg), "an unknown palette falls back to terminal");
    // Daylight is stored as dark source colours that the Light pass turns into its light design
    const contrast::Rgb dl = contrast::invertLightness({uipal::palette(7).bg.r, uipal::palette(7).bg.g, uipal::palette(7).bg.b});
    CHECK(pack(dl.r, dl.g, dl.b) == rgb8(243, 245, 250), "Daylight background after the turn-over");
    std::set<uint32_t> bgs, accents;
    for (int n = 0; n < uipal::kNumPalettes; n++) { bgs.insert(pack(uipal::palette(n).bg)); accents.insert(pack(uipal::palette(n).accent)); }
    CHECK(bgs.size() == (size_t)uipal::kNumPalettes, "every palette has its own background (%zu)", bgs.size());
    CHECK(accents.size() == (size_t)uipal::kNumPalettes, "every palette has its own accent (%zu)", accents.size());
    CHECK(uipal::metrics(5).frameRounding > uipal::metrics(3).frameRounding, "Studio is rounder than Dev A");
    CHECK(uipal::metrics(6).padY < uipal::metrics(5).padY, "Mono Pro is denser than Studio");
}

static void themes() {
    Look cur;   // the defaults of App: Dev A, dark, sidebar
    const Desc now = describe(cur);
    CHECK(now.bg == pack(uipal::palette(3).bg) && now.accent == pack(uipal::palette(3).accent), "the look in use is Dev A");
    std::set<uint32_t> seen;
    for (int t = 0; t < 8; t++) {
        const Desc d = previewFor(cur, Opt::Theme, t);
        seen.insert(d.bg ^ (d.accent * 31u));
        CHECK(ratio(d.text, d.bg) >= 4.5f, "palette %d: text legible on its background (%.2f)", t, ratio(d.text, d.bg));
        CHECK(!d.plotOnly && !d.strip, "palette %d: the whole window is shown", t);
    }
    CHECK(seen.size() == 8, "eight palettes, eight pictures (%zu)", seen.size());
    // Light: the lightness turned over, text stays legible on white
    const Desc light = previewFor(cur, Opt::Mode, 1);
    CHECK(light.light && lum(light.bg) > 0.7f && lum(light.text) < lum(light.bg), "Light is dark on white (bg %.2f)", lum(light.bg));
    CHECK(ratio(light.text, light.bg) >= 4.5f, "Light text ratio %.2f", ratio(light.text, light.bg));
    // High contrast: black, white and yellow, thicker outlines, the user's accent ignored
    Look own = cur; own.accentOn = true; own.accent[0] = 1; own.accent[1] = 0; own.accent[2] = 1;
    const Desc hc = previewFor(own, Opt::Mode, 2);
    CHECK(hc.highContrast && !hc.light, "High contrast is not turned over");
    CHECK(hc.bg == rgb8(0, 0, 0) && hc.text == rgb8(255, 255, 255) && hc.accent == pack(1.f, 0.90f, 0.f), "High contrast colours");
    CHECK(hc.border >= 2.f && ratio(hc.dim, hc.bg) >= 7.f, "High contrast outlines and dim text");
    CHECK(describe(own).accent == pack(1, 0, 1), "own accent colour applies outside High contrast");
    // Daylight is light even with Dark chosen; the frame pass runs for it
    Look day = with(cur, Opt::Theme, 7);
    CHECK(frameTurned(day) && describe(day).light && lum(describe(day).bg) > 0.8f, "Daylight is light");
    CHECK(!frameTurned(with(day, Opt::Mode, 2)), "High contrast wins over Light");
    // the classic interface
    const Desc cl = previewFor(cur, Opt::NewUi, 0);
    CHECK(cl.layout == kClassic && cl.accent == pack(0.45f, 0.72f, 0.82f) && cl.radius == 3.f, "classic interface");
}

static void lightPassUndone() {
    // a colour drawn as forFrame(c) in a frame the Light pass turns over appears exactly as c
    int bad = 0;
    for (int r = 0; r < 256; r += 5)
        for (int g = 0; g < 256; g += 7)
            for (int b = 0; b < 256; b += 11) {
                const uint32_t c = rgb8(r, g, b) & 0x80FFFFFFu;
                if (framePass(forFrame(c)) != c) bad++;
            }
    CHECK(bad == 0, "the Light pass is undone exactly (%d colours off)", bad);
}

static void layouts() {
    Look cur;
    std::set<int> seen;
    for (int v = 0; v < 8; v++) {
        const Desc d = previewFor(cur, Opt::Variant, v);
        CHECK(d.layout == v, "variant %d drawn as layout %d", v, d.layout);
        seen.insert(d.layout);
        CHECK(d.leds == (v == 7), "variant %d: round lamps only in Panel", v);
    }
    CHECK(seen.size() == 8, "eight layouts");
    CHECK(previewFor(cur, Opt::Unified, 1).unified && !previewFor(cur, Opt::Unified, 0).unified, "Classic and Unified");
    CHECK(previewFor(cur, Opt::Touch, 1).layout == kTouch, "touch interface");
    CHECK(previewFor(cur, Opt::Easy, 1).easy && !previewFor(cur, Opt::Easy, 0).easy, "Easy mode");
    const Desc ph = previewFor(cur, Opt::Variant, 3), pp = previewFor(cur, Opt::Variant, 7);
    CHECK(ph.chassis != pp.chassis && lum(ph.chassis) > 0.3f, "Faceplate and Panel chassis");
}

static void typography() {
    Look cur;
    const Desc now = previewFor(cur, Opt::Typography, 0), clean = previewFor(cur, Opt::Typography, 1), compact = previewFor(cur, Opt::Typography, 2);
    CHECK(!now.sansFont && clean.sansFont && compact.sansFont, "Clean and Compact use Inter");
    CHECK(clean.fontPx == 14.f && compact.fontPx == 13.f && now.fontPx == 13.f, "font sizes 13 / 14 / 13 (typography.cpp)");
    // the classic interface takes the font look's spacing too
    Look old = with(cur, Opt::NewUi, 0);
    const Desc oc = previewFor(old, Opt::Typography, 1), ok = previewFor(old, Opt::Typography, 2), on = previewFor(old, Opt::Typography, 0);
    CHECK(oc.spaceY > ok.spaceY && ok.spaceY > on.spaceY && oc.padY > ok.padY, "Clean airier than Compact, Compact than Current");
    // text size and touch scale
    CHECK(previewFor(cur, Opt::TextScale, 2.0).scale == 2.f && previewFor(cur, Opt::TextScale, 0.8).scale == 0.8f, "text size");
    CHECK(previewFor(cur, Opt::TextScale, 9).scale == 2.f && previewFor(cur, Opt::TextScale, 0.1).scale == 0.8f, "text size kept in 80..200 %%");
    const Desc t = previewFor(cur, Opt::Touch, 1);
    CHECK(t.scale > 1.3f && t.padY >= 1.f && t.radius >= 6.f && t.spaceY >= 8.f, "touch: bigger text, 48 px targets");
    CHECK((t.fontPx + 2 * t.padY) * t.scale >= 47.f, "touch rows are 48 px tall (%.1f)", (t.fontPx + 2 * t.padY) * t.scale);
}

static void colourMaps() {
    Look cur;
    for (int m = 0; m < cmap::NumMaps; m++) {
        const Desc d = previewFor(cur, Opt::WfMap, m);
        CHECK(d.wfMap == m && d.plotOnly && d.strip, "map %d previewed as a strip and a waterfall", m);
    }
    CHECK(previewFor(cur, Opt::WfMap, -1).wfMap == cmap::NavyHeat, "Dev A: following the palette is Navy heat");
    Look studio = with(cur, Opt::Theme, 5), mono = with(cur, Opt::Theme, 6), day = with(cur, Opt::Theme, 7), grey = with(cur, Opt::Theme, 2);
    studio.wfMap = mono.wfMap = day.wfMap = -1; grey.wfMap = -1;
    CHECK(describe(studio).wfMap == cmap::Magma && describe(mono).wfMap == cmap::Amber && describe(day).wfMap == cmap::Paper, "the 3.5 designs bring their maps");
    studio.wfMap = cmap::NavyHeat;
    CHECK(describe(studio).wfMap == cmap::Magma, "Navy heat (the default) gives way to Studio's map");
    studio.wfMap = cmap::Viridis;
    CHECK(describe(studio).wfMap == cmap::Viridis, "a map the user picked stays");
    CHECK(previewFor(cur, Opt::TraceColour, 1).trace == pack(1.f, 0.75f, 0.30f) && previewFor(cur, Opt::TraceColour, 1).plotOnly, "trace colour");
    CHECK(previewFor(cur, Opt::TunerStyle, 1).tunerSimple && !previewFor(cur, Opt::TunerStyle, 0).tunerSimple, "tuner style");
    CHECK(previewFor(cur, Opt::WfMap, 500).wfMap == cmap::NumMaps - 1, "an out-of-range map is clamped");
}

static void nothingElseChanges() {
    Look cur; cur.theme = 6; cur.variant = 2; cur.unified = true; cur.typography = 1; cur.textScale = 1.25f; cur.wfMap = 4; cur.mode = 1;
    const Look l = with(cur, Opt::Variant, 5);
    CHECK(l.variant == 5 && l.theme == 6 && l.unified && l.typography == 1 && l.textScale == 1.25f && l.wfMap == 4 && l.mode == 1, "only the layout changed");
    const Look t = with(cur, Opt::Theme, 0);
    CHECK(t.theme == 0 && t.variant == 2 && t.mode == 1, "only the palette changed");
    CHECK(with(with(cur, Opt::NewUi, 0), Opt::Theme, 4).newUi, "picking a palette is the new interface");
}

static void everyCombination() {
    int n = 0, bad = 0;
    for (int newUi = 0; newUi < 2; newUi++)
        for (int t = 0; t < 8; t++)
            for (int m = 0; m < 3; m++)
                for (int v = 0; v < 8; v++)
                    for (int ty = 0; ty < 3; ty++)
                        for (int touch = 0; touch < 2; touch++)
                            for (int easy = 0; easy < 2; easy++)
                                for (float s : {0.8f, 1.f, 2.f}) {
                                    Look l; l.newUi = newUi; l.theme = t; l.mode = m; l.variant = v; l.typography = ty; l.touch = touch; l.easy = easy; l.textScale = s;
                                    const Desc d = describe(l);
                                    n++;
                                    const bool ok = ratio(d.text, d.bg) >= 4.5f && ratio(d.text, d.rail) >= 3.f && d.scale >= 0.8f && d.scale <= 4.f &&
                                                    d.fontPx >= 13.f && d.padY >= 0.f && d.radius >= 0.f && d.wfMap >= 0 && d.wfMap < cmap::NumMaps &&
                                                    (d.layout == (touch ? kTouch : !newUi ? kClassic : v)) && d.light == frameTurned(l) &&
                                                    ((d.bg >> 24) == 255) && ratio(d.accent, d.bg) >= 1.5f;
                                    if (!ok && bad++ < 8)
                                        std::printf("  combination newUi %d palette %d theme %d layout %d font %d touch %d: text %.2f rail %.2f accent %.2f\n", newUi, t, m, v, ty, touch,
                                                    ratio(d.text, d.bg), ratio(d.text, d.rail), ratio(d.accent, d.bg));
                                }
    CHECK(bad == 0, "%d of %d combinations look wrong", bad, n);
}

int main() {
    paletteTable();
    themes();
    lightPassUndone();
    layouts();
    typography();
    colourMaps();
    nothingElseChanges();
    everyCombination();
    if (!failures) std::printf("ui preview OK\n");
    return failures ? 1 : 0;
}
