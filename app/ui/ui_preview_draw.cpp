// The live miniature of the hover preview (ui_preview.h, docs/UI-PREVIEW-4.0.md): drawn from the description of ui_preview_logic.h
// with ImGui draw lists, no pictures, nothing to ship, always the colours, spacing and layout the choice would really give. Text is
// drawn as short bars, so the High contrast pass (it recolours glyphs only) leaves the miniature alone and the Light pass is undone
// exactly by forFrame(). Only ImGui here, no App: tests/app/test_ui_preview_render.cpp rasterises it and checks the pixels.
#include "ui_preview.h"
#include "imgui.h"
#include "scale.h"
#include <algorithm>
#include <cmath>

namespace uiprev {

namespace {

// a small deterministic noise, so the miniature does not flicker from frame to frame
float hn(int i, int j) {
    uint32_t h = (uint32_t)i * 374761393u + (uint32_t)j * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFF) / 65535.f;
}

// the spectrum the miniature shows (0..1 across, level 0..1): a noise floor, a strong narrow carrier, a wide signal at the tuned spot,
// a weak one; `row` > 0 gives the waterfall rows (the weak signal comes and goes)
float level(float x, int row, int col) {
    auto bump = [](float x, float c, float w) { const float t = (x - c) / w; return std::exp(-t * t); };
    float v = 0.13f + 0.20f * hn(col, row) * hn(col + 3, row + 1);   // a speckled floor, as a real waterfall's noise
    v += 0.58f * bump(x, 0.22f, 0.012f);
    v += 0.38f * bump(x, 0.58f, 0.055f) * (0.75f + 0.25f * hn(col + 7, row));
    if (row < 0 || (row / 3) % 2 == 0) v += 0.30f * bump(x, 0.82f, 0.02f);
    return std::min(1.f, v);
}

struct Painter {
    ImDrawList* dl;
    const Desc& d;
    float u;       // pixels per real pixel at scale 1, the look's text scale included
    bool turned;   // pre-turn the colours: this frame is turned over afterwards
    ImU32 c(uint32_t col) const { return turned ? forFrame(col) : col; }
    ImU32 c(uint32_t col, float a) const { return c(withAlpha(col, a)); }
    float px(float v) const { return v * u; }
    float rowH() const { return std::max(3.f, px(d.fontPx + 2 * d.padY)); }
    float gap() const { return std::max(1.f, px(d.spaceY)); }
    float rad() const { return px(d.radius); }

    void rect(float x0, float y0, float x1, float y1, uint32_t col, float r = 0) const { if (x1 > x0 && y1 > y0) dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), c(col), r); }
    void outline(float x0, float y0, float x1, float y1, uint32_t col, float r = 0) const {
        if (x1 > x0 && y1 > y0) dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), c(col), r, 0, std::max(1.f, d.border * std::min(1.f, u * 2.f)));
    }
    // a line of text, drawn as a bar of the text's height (a sans look has rounder ends and words of mixed length)
    void text(float x, float yMid, float w, uint32_t col) const {
        const float h = std::max(1.f, px(d.fontPx) * 0.5f);
        if (!d.sansFont) { rect(x, yMid - h * 0.5f, x + w, yMid + h * 0.5f, col); return; }
        float cx = x;
        for (int k = 0; cx < x + w; k++) {   // words
            const float ww = std::min(x + w - cx, h * (2.5f + 3.f * hn(k, (int)x)));
            rect(cx, yMid - h * 0.5f, cx + ww, yMid + h * 0.5f, col, h * 0.5f);
            cx += ww + h * 0.8f;
        }
    }
    void button(float x0, float y0, float x1, float y1, bool accent) const {
        rect(x0, y0, x1, y1, accent ? d.accent : d.button, rad());
        if (!accent) outline(x0, y0, x1, y1, withAlpha(d.edge, 0.8f), rad());
        const float w = (x1 - x0) * 0.5f;
        text(x0 + (x1 - x0 - w) * 0.5f, (y0 + y1) * 0.5f, w, accent ? d.bg : d.text);
    }
    // a row of a settings panel: a dim label and a field
    void field(float x0, float y, float x1) const {
        const float h = rowH(), lw = (x1 - x0) * 0.38f;
        text(x0, y + h * 0.5f, lw * 0.8f, d.dim);
        rect(x0 + lw, y, x1, y + h, d.frame, rad());
        outline(x0 + lw, y, x1, y + h, withAlpha(d.edge, 0.6f), rad());
        text(x0 + lw + px(d.padX), y + h * 0.5f, (x1 - x0 - lw) * 0.45f, d.text);
    }
    void checkbox(float x0, float y, float x1, bool on) const {
        const float h = rowH(), s = h * 0.8f;
        rect(x0, y + (h - s) * 0.5f, x0 + s, y + (h + s) * 0.5f, d.frame, rad() * 0.5f);
        outline(x0, y + (h - s) * 0.5f, x0 + s, y + (h + s) * 0.5f, withAlpha(d.edge, 0.8f), rad() * 0.5f);
        if (on) rect(x0 + s * 0.25f, y + h * 0.5f - s * 0.25f, x0 + s * 0.75f, y + h * 0.5f + s * 0.25f, d.accent, rad() * 0.3f);
        text(x0 + s + px(4), y + h * 0.5f, (x1 - x0 - s) * 0.5f, d.text);
    }
    void lines(float x0, float y0, float x1, float y1, int seed) const {
        const float step = std::max(2.f, px(d.fontPx) * 0.5f + gap() + px(2));
        int k = 0;
        for (float y = y0 + step * 0.5f; y + step * 0.5f <= y1; y += step, k++) {
            const float w = (x1 - x0) * (0.35f + 0.55f * hn(k, seed));
            const float t = (x1 - x0) * 0.14f;   // a time stamp and the message
            text(x0, y, t, d.dim);
            text(x0 + t + px(6), y, std::max(0.f, w - t), k % 4 == 1 ? d.accent : d.text);
        }
    }
    void tabs(float x0, float y0, float x1, int n, int sel) const {
        const float h = rowH(), w = std::min((x1 - x0) / n, px(90));
        rect(x0, y0, x1, y0 + h, d.rail);
        for (int i = 0; i < n; i++) {
            const float a = x0 + i * w, b = a + w - px(1);
            if (i == sel) rect(a, y0, b, y0 + h, d.tabSel, rad());
            text(a + px(d.padX), y0 + h * 0.5f, (b - a) * 0.55f, i == sel ? d.tabText : d.dim);
        }
        rect(x0, y0 + h - std::max(1.f, px(1)), x1, y0 + h, d.edge);
    }
    void spectrum(float x0, float y0, float x1, float y1) const {
        if (x1 - x0 < 4 || y1 - y0 < 4) return;
        rect(x0, y0, x1, y1, d.plotBg);
        const ImU32 grid = c(d.edge, 0.55f);
        for (int k = 1; k < 4; k++) { const float x = std::floor(x0 + (x1 - x0) * k / 4) + 0.5f; dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), grid); }
        for (int k = 1; k < 3; k++) { const float y = std::floor(y0 + (y1 - y0) * k / 3) + 0.5f; dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), grid); }
        const float b0 = x0 + (x1 - x0) * 0.52f, b1 = x0 + (x1 - x0) * 0.64f, bc = x0 + (x1 - x0) * 0.58f;
        if (!d.tunerSimple) rect(b0, y0, b1, y1, withAlpha(d.accent, 0.16f));
        const int n = std::max(8, (int)((x1 - x0) / 1.5f));
        ImVec2 pts[400];
        const int m = std::min(n, 400);
        for (int i = 0; i < m; i++) {
            const float t = (float)i / (m - 1), v = level(t, -1, i);
            pts[i] = ImVec2(x0 + (x1 - x0) * t, y1 - 1 - (y1 - y0 - 3) * v * 0.92f);
        }
        for (int i = 0; i + 1 < m; i++)   // fill under the trace
            dl->AddQuadFilled(pts[i], pts[i + 1], ImVec2(pts[i + 1].x, y1), ImVec2(pts[i].x, y1), c(d.trace, 0.14f));
        dl->AddPolyline(pts, m, c(d.trace), 0, std::max(1.f, u * 2.4f));
        if (d.tunerSimple) {
            dl->AddLine(ImVec2(b0, y0), ImVec2(b0, y1), c(d.tuneEdge));
            dl->AddLine(ImVec2(b1, y0), ImVec2(b1, y1), c(d.tuneEdge));
            dl->AddLine(ImVec2(bc, y0), ImVec2(bc, y1), c(d.tuneCentre), std::max(1.f, u * 3.f));
        } else dl->AddLine(ImVec2(bc, y0), ImVec2(bc, y1), c(d.tuneEdge, 0.5f));
    }
    void waterfall(float x0, float y0, float x1, float y1) const {
        if (x1 - x0 < 4 || y1 - y0 < 2) return;
        const float cell = std::max(2.f, 3.f * gUi);
        int row = 0;
        for (float y = y0; y < y1; y += cell, row++) {
            int col = 0;
            for (float x = x0; x < x1; x += cell, col++) {
                const float t = (x + cell * 0.5f - x0) / (x1 - x0);
                const uint32_t mc = cmap::color(d.wfMap, std::max(0.f, (level(t, row, col) - 0.12f) * 1.15f));
                dl->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x1, x + cell), std::min(y1, y + cell)), c(mc));
            }
        }
        if (d.tunerSimple) {
            const float bc = x0 + (x1 - x0) * 0.58f;
            dl->AddLine(ImVec2(bc, y0), ImVec2(bc, y1), c(d.tuneCentre), std::max(1.f, u * 3.f));
        }
    }
    void strip(float x0, float y0, float x1, float y1) const {
        const int seg = 32;
        for (int s = 0; s < seg; s++) {
            const ImU32 a = c(cmap::color(d.wfMap, (float)s / seg)), b = c(cmap::color(d.wfMap, (float)(s + 1) / seg));
            dl->AddRectFilledMultiColor(ImVec2(x0 + (x1 - x0) * s / seg, y0), ImVec2(x0 + (x1 - x0) * (s + 1) / seg, y1), a, b, b, a);
        }
        outline(x0, y0, x1, y1, d.edge);
    }
    void plots(float x0, float y0, float x1, float y1, float specFrac) const {
        const float ys = y0 + (y1 - y0) * specFrac;
        spectrum(x0, y0, x1, ys);
        waterfall(x0, ys + std::max(1.f, px(2)), x1, y1);
        outline(x0, y0, x1, y1, withAlpha(d.edge, 0.8f));
    }
    void meters(float x0, float y0, float x1, float y1, int n) const {
        const float h = (y1 - y0) / n;
        for (int i = 0; i < n; i++) {
            const float y = y0 + i * h, bh = std::max(2.f, h * 0.38f);
            text(x0, y + h * 0.3f, (x1 - x0) * 0.35f, d.dim);
            rect(x0, y + h * 0.55f, x1, y + h * 0.55f + bh, d.frame, rad() * 0.5f);
            rect(x0, y + h * 0.55f, x0 + (x1 - x0) * (0.3f + 0.55f * hn(i, 3)), y + h * 0.55f + bh, i == 1 ? d.warn : d.accent, rad() * 0.5f);
        }
    }
    void led(float x, float y, float r, bool on) const {
        dl->AddCircleFilled(ImVec2(x, y), r, c(on ? d.accent : d.button));
        dl->AddCircle(ImVec2(x, y), r, c(d.edge, 0.9f));
    }
    void knob(float x, float y, float r) const {
        dl->AddCircleFilled(ImVec2(x, y), r, c(d.button));
        dl->AddCircle(ImVec2(x, y), r, c(d.edge));
        dl->AddLine(ImVec2(x, y), ImVec2(x + r * 0.6f, y - r * 0.6f), c(d.text), std::max(1.f, u * 2.f));
    }
    // the decoded views: tabs and message lines, or in Easy mode the Simple screen (a Start button and the key controls)
    void decoded(float x0, float y0, float x1, float y1) const {
        if (y1 - y0 < rowH()) return;
        if (d.easy) {
            const float h = rowH() * 1.6f, g = gap();
            button(x0 + px(8), y0 + g, x1 - px(8), y0 + g + h, true);
            float y = y0 + 2 * g + h;
            for (int k = 0; k < 3 && y + rowH() <= y1; k++, y += rowH() + g) field(x0 + px(8), y, x1 - px(8));
            return;
        }
        tabs(x0, y0, x1, 4, 0);
        lines(x0 + px(6), y0 + rowH() + gap(), x1 - px(6), y1, (int)x0);
    }
    // the top bar: run button, the frequency in large digits, what is received
    float topBar(float x0, float y0, float x1, float big) const {
        const float h = rowH() * big;
        rect(x0, y0, x1, y0 + h, d.rail);
        const float g = gap() + px(3);
        button(x0 + g, y0 + g, x0 + g + h * 1.4f, y0 + h - g, true);
        const float dh = std::max(2.f, (h - 2 * g) * 0.62f);
        rect(x0 + 2 * g + h * 1.6f, y0 + (h - dh) * 0.5f, x0 + 2 * g + h * 1.6f + (x1 - x0) * 0.30f, y0 + (h + dh) * 0.5f, d.bright, std::min(dh * 0.25f, rad()));
        text(x1 - (x1 - x0) * 0.25f, y0 + h * 0.5f, (x1 - x0) * 0.2f, d.dim);
        rect(x0, y0 + h - std::max(1.f, px(1)), x1, y0 + h, d.edge);
        return y0 + h;
    }
    void statusBar(float x0, float y0, float x1, float y1) const {
        rect(x0, y0, x1, y1, d.rail);
        rect(x0, y0, x1, y0 + std::max(1.f, px(1)), d.edge);
        text(x0 + px(6), (y0 + y1) * 0.5f, (x1 - x0) * 0.22f, d.dim);
        text(x0 + (x1 - x0) * 0.55f, (y0 + y1) * 0.5f, (x1 - x0) * 0.18f, d.dim);
        dl->AddCircleFilled(ImVec2(x1 - px(10), (y0 + y1) * 0.5f), std::max(1.5f, (y1 - y0) * 0.22f), c(d.ok));
    }
    void sidebar(float x0, float y0, float x1, float y1) const {
        rect(x0, y0, x1, y1, d.rail);
        rect(x1 - std::max(1.f, px(1)), y0, x1, y1, d.edge);
        float y = y0 + gap() + px(2);
        const float l = x0 + px(8), r = x1 - px(8);
        for (int s = 0; s < 3 && y + rowH() < y1; s++) {
            text(l, y + rowH() * 0.5f, (r - l) * 0.45f, d.bright);   // a section heading
            y += rowH() + gap();
            for (int k = 0; k < 3 && y + rowH() < y1; k++, y += rowH() + gap()) {
                if (s == 1 && k == 2) checkbox(l, y, r, true);
                else if (s == 2 && k == 0) button(l, y, l + (r - l) * 0.55f, y + rowH(), false);
                else field(l, y, r);
            }
            y += gap();
        }
    }

    void body(float x0, float y0, float x1, float y1) const {
        const float g = std::max(2.f, px(4));
        switch (d.layout) {
        case kClassic: {   // the classic interface: a toolbar of buttons, the tabs, the spectrum and waterfall in the first tab
            const float h = rowH() + 2 * gap();
            rect(x0, y0, x1, y0 + h, d.rail);
            float x = x0 + g;
            for (int k = 0; k < 4; k++, x += (x1 - x0) * 0.13f) button(x, y0 + gap(), x + (x1 - x0) * 0.11f, y0 + h - gap(), k == 0);
            text(x + g, y0 + h * 0.5f, (x1 - x0) * 0.25f, d.bright);
            tabs(x0, y0 + h, x1, 5, 0);
            const float t = y0 + h + rowH();
            if (d.easy) { plots(x0 + g, t + g, x1 - g, t + (y1 - t) * 0.55f, 0.5f); decoded(x0, t + (y1 - t) * 0.55f + g, x1, y1); }
            else plots(x0 + g, t + g, x1 - g, y1 - g, 0.48f);
            break;
        }
        case kScope: {   // a control ribbon and a big spectrum and waterfall
            y0 = topBar(x0, y0, x1, 1.6f);
            const float h = rowH() + 2 * gap();
            float x = x0 + g;
            for (int k = 0; k < 6; k++, x += (x1 - x0) * 0.14f) button(x, y0 + gap(), x + (x1 - x0) * 0.12f, y0 + h - gap(), false);
            plots(x0 + g, y0 + h, x1 - g, y1 - g, 0.42f);
            break;
        }
        case kTiles: {   // cards: spectrum, waterfall, meters, the decoded text
            y0 = topBar(x0, y0, x1, 1.6f);
            const float xm = (x0 + x1) * 0.5f, ym = (y0 + y1) * 0.5f;
            const float bx[4][4] = {{x0 + g, y0 + g, xm - g * 0.5f, ym - g * 0.5f}, {xm + g * 0.5f, y0 + g, x1 - g, ym - g * 0.5f},
                                    {x0 + g, ym + g * 0.5f, xm - g * 0.5f, y1 - g}, {xm + g * 0.5f, ym + g * 0.5f, x1 - g, y1 - g}};
            for (int k = 0; k < 4; k++) {
                const float* b = bx[k];
                rect(b[0], b[1], b[2], b[3], d.rail, rad() * 1.4f);
                outline(b[0], b[1], b[2], b[3], d.edge, rad() * 1.4f);
                const float p = std::max(2.f, px(5));
                if (k == 0) spectrum(b[0] + p, b[1] + p, b[2] - p, b[3] - p);
                else if (k == 1) waterfall(b[0] + p, b[1] + p, b[2] - p, b[3] - p);
                else if (k == 2) meters(b[0] + p, b[1] + p, b[2] - p, b[3] - p, 3);
                else if (d.easy) decoded(b[0], b[1] + p, b[2], b[3] - p);
                else lines(b[0] + p, b[1] + p, b[2] - p, b[3] - p, 5);
            }
            break;
        }
        case kFaceplate: case kPanel: {   // an instrument in a chassis
            rect(x0, y0, x1, y1, d.chassis);
            const bool panel = d.layout == kPanel;
            const float split = x0 + (x1 - x0) * (panel ? 0.70f : 0.66f);
            if (panel) {   // the mode keys on the right, each with a lamp
                const int n = 8;
                const float kh = (y1 - y0 - 2 * g) / n;
                for (int k = 0; k < n; k++) {
                    const float ky = y0 + g + k * kh;
                    button(split + g, ky + 1, x1 - g - kh * 0.7f, ky + kh - 1, false);
                    led(x1 - g - kh * 0.35f, ky + kh * 0.5f, std::max(1.5f, kh * 0.18f), k == 2);
                }
                const float ly = y1 - g - rowH();
                plots(x0 + g, y0 + g + rowH() + g, split - g, ly - g, 0.45f);
                field(x0 + g, y0 + g, split - g);
                for (int k = 0; k < 6; k++) led(x0 + g + rowH() * (0.6f + 1.4f * k), ly + rowH() * 0.5f, std::max(1.5f, rowH() * 0.3f), k % 3 == 0);
            } else {   // the display on the left, knobs and keys on the right, an analysis strip below
                const float ya = y0 + (y1 - y0) * 0.72f;
                rect(x0 + g, y0 + g, split - g, ya - g, d.plotBg, rad());
                outline(x0 + g, y0 + g, split - g, ya - g, d.edge, rad());
                const float ih = rowH() * 1.3f;
                rect(x0 + 2 * g, y0 + 2 * g, x0 + 2 * g + (split - x0) * 0.45f, y0 + 2 * g + ih * 0.7f, d.accent, rad() * 0.5f);
                plots(x0 + 2 * g, y0 + 3 * g + ih, split - 2 * g, ya - 2 * g, 0.45f);
                const float kr = std::min((x1 - split) * 0.2f, (ya - y0) * 0.16f);
                for (int k = 0; k < 4; k++) knob(split + (x1 - split) * (k % 2 ? 0.72f : 0.30f), y0 + g + kr + (k / 2) * (kr * 2.6f), kr);
                button(split + g, ya - g - rowH(), x1 - g, ya - g, true);
                meters(x0 + g, ya, x1 - g, y1 - g, 2);
            }
            break;
        }
        case kCinema: {   // the picture fills the window, a thin overlay row of readouts
            y0 = topBar(x0, y0, x1, 1.6f);
            plots(x0 + g, y0 + g, x1 - g, y1 - g, 0.38f);
            rect(x0 + 2 * g, y0 + 2 * g, x0 + (x1 - x0) * 0.45f, y0 + 2 * g + rowH(), withAlpha(d.rail, 0.8f), rad());
            text(x0 + 3 * g, y0 + 2 * g + rowH() * 0.5f, (x1 - x0) * 0.33f, d.text);
            break;
        }
        case kMeters: {   // the spectrum on top, a grid of meters below
            y0 = topBar(x0, y0, x1, 1.6f);
            const float ym = y0 + (y1 - y0) * 0.58f;
            plots(x0 + g, y0 + g, x1 - g, ym, 0.55f);
            const float xm = (x0 + x1) * 0.5f;
            meters(x0 + g, ym + g, xm - g, y1 - g, 3);
            meters(xm + g, ym + g, x1 - g, y1 - g, 3);
            break;
        }
        case kDock: {   // spectrum and waterfall, an IDE-style tabbed panel along the bottom
            y0 = topBar(x0, y0, x1, 1.6f);
            const float ym = y0 + (y1 - y0) * 0.58f;
            plots(x0 + g, y0 + g, x1 - g, ym, 0.5f);
            decoded(x0, ym + g, x1, y1);
            break;
        }
        case kTouch: {   // big digits, the spectrum, a bottom dock of big buttons
            y0 = topBar(x0, y0, x1, 1.15f);
            const float bh = rowH() * 0.9f, yd = y1 - bh - 2 * g;
            if (d.easy) { plots(x0 + g, y0 + g, x1 - g, y0 + (yd - y0) * 0.55f, 0.5f); decoded(x0, y0 + (yd - y0) * 0.55f + g, x1, yd); }
            else plots(x0 + g, y0 + g, x1 - g, yd - g, 0.45f);
            rect(x0, yd, x1, y1, d.rail);
            const float bw = (x1 - x0 - 5 * g) / 4;
            for (int k = 0; k < 4; k++) button(x0 + g + k * (bw + g), yd + g, x0 + g + k * (bw + g) + bw, y1 - g, k == 0);
            break;
        }
        default: {   // the side panel, and the spectrum with the decoded views (Unified: above them; Classic: in their own tab)
            y0 = topBar(x0, y0, x1, 1.6f);
            const float sx = x0 + (x1 - x0) * 0.30f;
            sidebar(x0, y0, sx, y1);
            if (d.unified) {
                const float ym = y0 + (y1 - y0) * 0.52f;
                plots(sx + g, y0 + g, x1 - g, ym, 0.5f);
                decoded(sx, ym + g, x1, y1);
            } else if (d.easy) {
                const float ym = y0 + (y1 - y0) * 0.5f;
                plots(sx + g, y0 + g, x1 - g, ym, 0.5f);
                decoded(sx, ym + g, x1, y1);
            } else {
                tabs(sx, y0, x1, 4, 0);
                plots(sx + g, y0 + rowH() + g, x1 - g, y1 - g, 0.48f);
            }
            break;
        }
        }
    }
};

}   // namespace

void draw(ImDrawList* dl, float x, float y, float w, float h, const Desc& d, float unit, bool turned) {
    Painter P{dl, d, unit, turned};
    dl->PushClipRect(ImVec2(x, y), ImVec2(x + w, y + h), true);
    if (d.plotOnly) {
        float top = y;
        P.rect(x, y, x + w, y + h, d.bg);
        if (d.strip) { P.strip(x, y, x + w, y + std::max(6.f, 12 * gUi)); top = y + std::max(6.f, 12 * gUi) + std::max(2.f, 4 * gUi); }
        P.plots(x, top, x + w, y + h, 0.42f);
        dl->PopClipRect();
        return;
    }
    const float r = std::min(10.f * gUi, P.rad() * 1.4f);
    P.rect(x, y, x + w, y + h, d.bg, r);
    // the window's title bar: three lamps and the title
    const float th = std::max(6.f, P.rowH() + P.gap());
    P.rect(x, y, x + w, y + th, d.rail, r);
    for (int k = 0; k < 3; k++) dl->AddCircleFilled(ImVec2(x + th * (0.5f + 0.55f * k), y + th * 0.5f), std::max(1.5f, th * 0.16f), P.c(d.dim));
    P.text(x + w * 0.42f, y + th * 0.5f, w * 0.16f, d.dim);
    P.rect(x, y + th - std::max(1.f, P.px(1)), x + w, y + th, d.edge);
    const float sh = std::max(4.f, P.rowH());
    P.body(x, y + th, x + w, y + h - sh);
    P.statusBar(x, y + h - sh, x + w, y + h);
    P.outline(x, y, x + w, y + h, d.edge, r);
    dl->PopClipRect();
}

}   // namespace uiprev
