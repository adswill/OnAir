// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The tuning marker (the VFO) over the spectrum and the waterfall: a slim centre line with a dark halo (readable on a bright waterfall
// as on the black plate), a small cap at the top that opens into a tab with a label (the demodulator and width, the offset while
// dragging), the passband as a soft band in the theme's accent colour (a gradient on the spectrum, a light veil on the waterfall) with
// thin edge lines, and grab handles at the movable edges that fade in and grow on hover. The edges, the handles and the tab ease in
// over about 120 ms; a band only a few pixels wide folds into the centre line (with hysteresis, so it does not flicker at the limit).
// Only drawing, into an ImDrawList, no allocation: the callers keep the hit-testing and the dragging (general_ui.cpp, plots.cpp).
#pragma once
#include "imgui.h"
#include <algorithm>
#include <cmath>

namespace vmark {

// What eases between frames (one per plot the marker is drawn on)
struct Anim {
    bool init = false;
    double lo = 0, hi = 0;    // the edges, in the caller's offset unit (Hz)
    float hov[3] = {0, 0, 0}; // low edge, high edge, body: 0 .. 1
    float tab = 0;            // the label tab, 0 .. 1
    float band = 1;           // the band (0 when folded into the centre line)
    bool folded = false;
};

enum Part { None = -1, EdgeLo = 0, EdgeHi = 1, Body = 2 };

struct In {
    ImVec2 pmin, psize;          // the plot area on the screen
    double xC = 0;               // the tuned frequency, screen pixels
    double lo = 0, hi = 0;       // the passband edges as offsets from the tuned frequency (any unit, e.g. Hz; lo <= hi)
    double pxPerUnit = 0;        // screen pixels per offset unit
    bool spectrum = true;        // the spectrum (gradient band, cap on the line) or the waterfall (a veil)
    bool canLo = true, canHi = true;   // which edges can be dragged (USB / LSB: the outer one only): handles on these
    int hover = None;            // the part under the mouse (Part)
    int drag = None;             // the part held
    const char* label = nullptr; // the tab's text, shown on hover or drag (nullptr: none)
    ImVec4 accent = ImVec4(0.45f, 0.72f, 0.82f, 1);   // the theme's accent
    ImVec4 tabBg = ImVec4(0.075f, 0.078f, 0.083f, 1), tabText = ImVec4(0.82f, 0.84f, 0.85f, 1);
    float sc = 1.f;              // interface scale (gUi)
    float dt = 1.f / 60;         // seconds since the last frame
};

inline ImU32 col(ImVec4 c, float a) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, std::clamp(c.w * a, 0.f, 1.f))); }
inline ImVec4 mix(ImVec4 a, ImVec4 b, float t) { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t); }
// exponential ease towards a target: 95 % of the way in about 3 tau (tau 40 ms: ~120 ms)
inline float easeK(float dt, float tau = 0.04f) { return dt <= 0 ? 1.f : 1.f - std::exp(-std::min(dt, 0.25f) / tau); }
inline float approach(float v, float to, float k) { const float r = v + (to - v) * k; return std::fabs(r - to) < 0.002f ? to : r; }

// a vertical 1-device-pixel-crisp line
inline float crisp(double x) { return std::floor((float)x) + 0.5f; }

inline void draw(ImDrawList* dl, const In& I, Anim& A) {
    if (I.psize.x <= 0 || I.psize.y <= 0 || !(I.pxPerUnit > 0) || !std::isfinite(I.xC)) return;
    const float sc = std::max(0.5f, I.sc), k = easeK(I.dt);
    // the edges ease to a new width (a preset, another demodulator), but follow at once while one is dragged
    const bool dragEdge = I.drag == EdgeLo || I.drag == EdgeHi;
    if (!A.init || dragEdge) { A.lo = I.lo; A.hi = I.hi; A.init = true; }
    else {
        A.lo += (I.lo - A.lo) * k; A.hi += (I.hi - A.hi) * k;
        const double eps = 0.25 / I.pxPerUnit;   // a quarter pixel: settled
        if (std::fabs(A.lo - I.lo) < eps) A.lo = I.lo;
        if (std::fabs(A.hi - I.hi) < eps) A.hi = I.hi;
    }
    for (int p = 0; p < 3; p++) A.hov[p] = approach(A.hov[p], (I.hover == p || I.drag == p) ? 1.f : 0.f, k);
    A.tab = approach(A.tab, I.label && (I.hover != None || I.drag != None) ? 1.f : 0.f, k);

    const float y0 = I.pmin.y, y1 = I.pmin.y + I.psize.y, xr = I.pmin.x + I.psize.x;
    const float xc = crisp(I.xC);
    const float xl = (float)(I.xC + A.lo * I.pxPerUnit), xh = (float)(I.xC + A.hi * I.pxPerUnit);
    const float w = xh - xl;
    // fold a band of a few pixels into the line; unfold only well past it (no flicker while zooming across the limit)
    if (!A.folded && w < 3.f * sc) A.folded = true;
    else if (A.folded && w > 6.f * sc) A.folded = false;
    A.band = approach(A.band, A.folded ? 0.f : 1.f, k);

    const ImVec4 acc = I.accent;
    const ImVec4 lineC = mix(acc, ImVec4(1, 1, 1, 1), 0.70f);   // the centre: the accent, lightened
    const ImVec4 edgeC = mix(acc, ImVec4(1, 1, 1, 1), 0.20f);
    const ImVec4 shade(0, 0, 0, 1);
    const float t1 = std::max(1.f, std::round(sc));   // a hairline, one device pixel per interface scale step

    dl->PushClipRect(I.pmin, ImVec2(xr, y1), true);
    if (A.band > 0.01f) {
        const float ba = A.band, body = A.hov[Body];
        const float bl = std::floor(xl), bh = std::max(bl + 1.f, std::floor(xh) + 1.f);
        if (I.spectrum) {   // the band: stronger at the top, fading towards the noise floor
            const float top = (0.26f + 0.08f * body) * ba, bot = (0.07f + 0.04f * body) * ba;
            dl->AddRectFilledMultiColor(ImVec2(bl, y0), ImVec2(bh, y1), col(acc, top), col(acc, top), col(acc, bot), col(acc, bot));
        } else {
            dl->AddRectFilled(ImVec2(bl, y0), ImVec2(bh, y1), col(acc, (0.09f + 0.05f * body) * ba));
        }
        for (int e = 0; e < 2; e++) {   // the edges: a thin accent line with a faint dark line outside it
            const float x = crisp(e ? xh : xl), h = A.hov[e];
            const float out = e ? x + t1 : x - t1;
            dl->AddLine(ImVec2(out, y0), ImVec2(out, y1), col(shade, (I.spectrum ? 0.30f : 0.40f) * ba), t1);
            dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), col(edgeC, (0.62f + 0.38f * h) * ba), t1 * (1.f + h));
        }
        for (int e = 0; e < 2; e++) {   // the handles: a pill on the movable edges that fades in and grows on hover
            const float h = A.hov[e] * ba;
            if (h < 0.02f || !(e ? I.canHi : I.canLo)) continue;
            const float x = crisp(e ? xh : xl), ym = y0 + I.psize.y * 0.5f;
            const float hw = (1.5f + 1.5f * h) * sc, hh = std::min(I.psize.y * 0.4f, (9.f + 7.f * h) * sc);
            const ImVec2 a(x - hw, ym - hh), b(x + hw, ym + hh);
            dl->AddRectFilled(ImVec2(a.x - t1, a.y - t1), ImVec2(b.x + t1, b.y + t1), col(shade, 0.55f * h), hw + t1);
            dl->AddRectFilled(a, b, col(lineC, h), hw);
            if (h > 0.5f) for (int g = -1; g <= 1; g++) {   // three grip notches
                const float gy = ym + g * 3.f * sc;
                dl->AddLine(ImVec2(x - hw + sc, gy), ImVec2(x + hw - sc, gy), col(acc, (h - 0.5f) * 2.f), std::max(1.f, sc * 0.75f));
            }
        }
    }
    // the centre line: a dark halo under a light hairline
    const float hb = A.hov[Body];
    dl->AddLine(ImVec2(xc, y0), ImVec2(xc, y1), col(shade, 0.45f), t1 * 3.f);
    dl->AddLine(ImVec2(xc, y0), ImVec2(xc, y1), col(lineC, 0.92f + 0.08f * hb), t1 * (1.f + 0.5f * hb));
    if (I.spectrum || A.tab > 0.01f) {   // the cap: a small flag pointing down onto the line (it opens into the tab)
        const float cw = 5.f * sc, chh = 6.f * sc;
        const ImVec2 p1(xc - cw, y0), p2(xc + cw, y0), p3(xc, y0 + chh);
        dl->AddTriangleFilled(ImVec2(p1.x - t1, p1.y), ImVec2(p2.x + t1, p2.y), ImVec2(p3.x, p3.y + 1.5f * t1), col(shade, 0.5f));
        dl->AddTriangleFilled(p1, p2, p3, col(lineC, 1.f));
    }
    if (A.tab > 0.01f && I.label && *I.label) {   // the tab with its label, kept inside the plot
        ImFont* f = ImGui::GetFont();
        const float fs = ImGui::GetFontSize();
        const ImVec2 ts = f->CalcTextSizeA(fs, 1e9f, 0.f, I.label);
        const float padX = 6.f * sc, padY = 2.f * sc, r = 3.f * sc;
        const float tw = ts.x + 2 * padX, th = ts.y + 2 * padY;
        float tx = std::clamp(xc - tw * 0.5f, I.pmin.x + 2.f * sc, std::max(I.pmin.x + 2.f * sc, xr - tw - 2.f * sc));
        const float ty = y0 + 5.f * sc - (1.f - A.tab) * 4.f * sc;   // slides down a little as it appears
        const float a = A.tab;
        dl->AddRectFilled(ImVec2(tx - t1, ty - t1), ImVec2(tx + tw + t1, ty + th + t1), col(shade, 0.35f * a), r + t1);
        dl->AddRectFilled(ImVec2(tx, ty), ImVec2(tx + tw, ty + th), col(I.tabBg, 0.94f * a), r);
        dl->AddRect(ImVec2(tx, ty), ImVec2(tx + tw, ty + th), col(acc, 0.85f * a), r, 0, t1);
        dl->AddText(f, fs, ImVec2(std::round(tx + padX), std::round(ty + padY)), col(I.tabText, a), I.label);
    }
    dl->PopClipRect();
}

}   // namespace vmark
