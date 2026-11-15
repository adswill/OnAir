// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Detached panels (issue #242): the spectrum, the waterfall and the message log as OS windows of their own, for a second monitor.
// View > Allow detaching panels (experimental), then View > Detach. Each is a read-only mirror of the main window's data (no input
// into the receiver). It uses the technique of the video pop-out (popout.cpp): a GLFW window drawn by the main graphics back end from the
// main loop, with one ImGui context; the picture is a separate draw list handed to the renderer, so no ImGui window or viewport is
// involved and the main window's layout, docking and keyboard shortcuts are untouched. Desktop only; not made with --hidden.
#include "../app.h"
#include "detach.h"
#include <cmath>
#include <cstdio>

namespace {

struct Win {
    GLFWwindow* w = nullptr;
    gfx::Surface* surf = nullptr;
    ImDrawList* dl = nullptr;
    ImDrawData dd;
    gfx::Image* img = nullptr;       // the waterfall's picture
    std::vector<uint32_t> px;
    double imgT = 0;
    long long imgRows = -1;
};
Win gWin[detach::kCount];

constexpr int kImgW = 512, kImgH = 256;

const char* keyOf(int k, const char* what) {
    static char b[3][24];
    snprintf(b[k], sizeof b[k], "det%s%d", what, k);
    return b[k];
}

void saveGeometry(int k) {
    Win& d = gWin[k];
    if (!d.w) return;
    int x = 0, y = 0, w = 0, h = 0;
    if (!gWayland) glfwGetWindowPos(d.w, &x, &y);
    glfwGetWindowSize(d.w, &w, &h);
    if (w <= 0 || h <= 0) return;
    plat::Prefs& p = plat::prefs();
    if (!gWayland) { p.setI(keyOf(k, "X"), x); p.setI(keyOf(k, "Y"), y); }
    p.setI(keyOf(k, "W"), w); p.setI(keyOf(k, "H"), h);
    p.flush();
}

void closeOne(int k) {
    Win& d = gWin[k];
    if (!d.w) return;
    saveGeometry(k);
    delete d.img; d.img = nullptr;
    delete d.surf; d.surf = nullptr;
    glfwDestroyWindow(d.w); d.w = nullptr;
    d.dd.Clear();
    d.imgRows = -1;
}

bool openOne(int k) {
    plat::Prefs& p = plat::prefs();
    int w = (int)p.getI(keyOf(k, "W"), (long)(640 * gUi)), h = (int)p.getI(keyOf(k, "H"), (long)(360 * gUi));
    w = std::max(200, std::min(w, 8192)); h = std::max(120, std::min(h, 8192));
    glfwDefaultWindowHints();
    gfx::windowHints();
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);   // the main window keeps the keyboard
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow* win = glfwCreateWindow(w, h, detach::title(k), nullptr, gGfx->shareWindow());
    glfwDefaultWindowHints();
    if (!win) return false;
    Win& d = gWin[k];
    d.w = win;
    glfwSetWindowSizeLimits(win, 200, 120, GLFW_DONT_CARE, GLFW_DONT_CARE);
    if (!gWayland && p.has(keyOf(k, "X")) && p.has(keyOf(k, "Y"))) {   // back where it was if that is still on a screen
        const int x = (int)p.getI(keyOf(k, "X"), 0), y = (int)p.getI(keyOf(k, "Y"), 0);
        bool onScreen = false;
        for (GLFWmonitor* m : monitors()) {
            int mx = 0, my = 0, mw = 0, mh = 0;
            glfwGetMonitorWorkarea(m, &mx, &my, &mw, &mh);
            if (std::min(x + w, mx + mw) - std::max(x, mx) >= std::min(w, 100) && y >= my && y < my + mh - 40) { onScreen = true; break; }
        }
        if (onScreen) glfwSetWindowPos(win, x, y);
    }
    d.surf = gGfx->createSurface(win);
    if (!d.surf) { glfwDestroyWindow(win); d.w = nullptr; return false; }
    glfwShowWindow(win);
    return true;
}

// the spectrum: the trace of the main plot with a frequency scale and a dB scale
void drawSpectrum(App& a, ImDrawList* dl, float W, float H) {
    const std::vector<float>& v = !a.specView.empty() ? a.specView : a.spec.dbfs;
    const float lm = 44 * gUi, bm = 22 * gUi, tm = 6 * gUi, rm = 8 * gUi;
    const float x0 = lm, x1 = W - rm, y0 = tm, y1 = H - bm;
    if (x1 <= x0 + 10 || y1 <= y0 + 10) return;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(10, 14, 20, 255));
    const float lo = a.yMin, hi = a.yMax > a.yMin + 1 ? a.yMax : a.yMin + 1;
    for (int i = 0; i <= 5; i++) {   // dB grid
        const float db = lo + (hi - lo) * (float)i / 5, y = y1 - (y1 - y0) * (float)i / 5;
        dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), IM_COL32(60, 70, 85, 255));
        char t[16]; snprintf(t, sizeof t, "%.0f", db);
        dl->AddText(ImVec2(4 * gUi, y - ImGui::GetFontSize() * 0.5f), IM_COL32(170, 180, 195, 255), t);
    }
    double c = std::isfinite(a.wf.histC) ? a.wf.histC : a.freqMhz, span = a.wf.histFs > 0 ? a.wf.histFs : 0;
    for (int i = 0; i <= 4; i++) {   // frequency grid
        const float x = x0 + (x1 - x0) * (float)i / 4;
        dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), IM_COL32(60, 70, 85, 255));
        char t[32];
        if (span > 0) snprintf(t, sizeof t, "%.3f", c - span * 0.5 + span * i / 4);
        else snprintf(t, sizeof t, "%d%%", i * 25);
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(std::min(std::max(x - ts.x * 0.5f, 0.f), W - ts.x), y1 + 3 * gUi), IM_COL32(170, 180, 195, 255), t);
    }
    if (v.size() >= 2) {
        const int n = (int)v.size(), px = (int)(x1 - x0);
        std::vector<ImVec2> pts;
        pts.reserve((size_t)px + 2);
        pts.push_back(ImVec2(x0, y1));
        for (int i = 0; i < px; i++) {   // the highest level of the bins under each pixel column
            const int b0 = (int)((long long)i * n / px), b1 = std::max(b0 + 1, (int)((long long)(i + 1) * n / px));
            float m = -1e9f;
            for (int b = b0; b < b1 && b < n; b++) m = std::max(m, v[(size_t)b]);
            const float f = std::min(1.f, std::max(0.f, (m - lo) / (hi - lo)));
            pts.push_back(ImVec2(x0 + (float)i, y1 - f * (y1 - y0)));
        }
        pts.push_back(ImVec2(x1, y1));
        dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
        dl->AddConvexPolyFilled(pts.data(), (int)pts.size(), IM_COL32(40, 120, 200, 70));
        dl->AddPolyline(pts.data() + 1, (int)pts.size() - 2, IM_COL32(90, 190, 255, 255), 0, 1.2f * gUi);
        dl->PopClipRect();
    }
    {
        char t[48]; snprintf(t, sizeof t, "%.4f MHz", a.freqMhz);
        dl->AddText(ImVec2(x0 + 6 * gUi, y0 + 4 * gUi), IM_COL32(240, 240, 240, 255), t);
    }
}

// the waterfall: the history's levels through the colour table, newest row on top, the whole band
void drawWaterfall(App& a, Win& d, ImDrawList* dl, float W, float H) {
    const Waterfall& wf = a.wf;
    if (!d.img) {
        d.img = gGfx->createImage(kImgW, kImgH, 0xFF000000u);
        d.px.assign((size_t)kImgW * kImgH, 0xFF000000u);
        d.imgRows = -1;
    }
    if (d.img && wf.cols > 0 && wf.lv.size() >= (size_t)wf.cols * Waterfall::H && wf.rows > 0 && (wf.rows != d.imgRows || glfwGetTime() - d.imgT > 1.0) && glfwGetTime() - d.imgT > 0.05) {
        d.imgT = glfwGetTime(); d.imgRows = wf.rows;
        const int cols = wf.cols, H0 = Waterfall::H;
        for (int r = 0; r < kImgH; r++) {
            const int src = (wf.writeRow + r * H0 / kImgH) % H0;   // writeRow: the newest, older rows follow, wrapping
            const uint8_t* l = &wf.lv[(size_t)src * cols];
            uint32_t* o = &d.px[(size_t)r * kImgW];
            for (int c = 0; c < kImgW; c++) {
                const int c0 = (int)((long long)c * cols / kImgW), c1 = std::max(c0 + 1, (int)((long long)(c + 1) * cols / kImgW));
                uint8_t m = 0;
                for (int i = c0; i < c1 && i < cols; i++) m = std::max(m, l[i]);   // the highest level, so narrow signals stay
                o[c] = wf.lut[m];
            }
        }
        d.img->update(0, 0, kImgW, kImgH, d.px.data());
    }
    const float m = 4 * gUi;
    if (d.img && d.imgRows >= 0) dl->AddImage(d.img->texture(), ImVec2(m, m), ImVec2(W - m, H - m));
    else dl->AddText(ImVec2(m * 2, m * 2), IM_COL32(128, 128, 128, 255), "no data yet");
}

// the message log: the newest lines that fit
void drawMessages(App& a, ImDrawList* dl, float W, float H) {
    size_t total = 0;
    const std::vector<std::string> lines = a.engine.logSnapshot(total);
    const float lh = ImGui::GetFontSize() + 2 * gUi, m = 6 * gUi;
    const int fit = std::max(1, (int)((H - 2 * m) / lh));
    const int first = std::max(0, (int)lines.size() - fit);
    dl->PushClipRect(ImVec2(0, 0), ImVec2(W, H), true);
    float y = m;
    for (int i = first; i < (int)lines.size(); i++, y += lh) dl->AddText(ImVec2(m, y), IM_COL32(210, 216, 225, 255), lines[(size_t)i].c_str());
    dl->PopClipRect();
}

void frameOne(App& a, int k) {
    Win& d = gWin[k];
    int ww = 0, wh = 0, fw = 0, fh = 0;
    glfwGetWindowSize(d.w, &ww, &wh);
    glfwGetFramebufferSize(d.w, &fw, &fh);
    d.dd.Clear();
    if (ww <= 0 || wh <= 0 || fw <= 0 || fh <= 0) return;   // minimised
    if (!d.dl) d.dl = IM_NEW(ImDrawList)(ImGui::GetDrawListSharedData());
    d.dl->_ResetForNewFrame();
    d.dl->PushClipRect(ImVec2(0, 0), ImVec2((float)ww, (float)wh));
    d.dl->PushTexture(ImGui::GetIO().Fonts->TexRef);
    if (k == detach::Spectrum) drawSpectrum(a, d.dl, (float)ww, (float)wh);
    else if (k == detach::Waterfall) drawWaterfall(a, d, d.dl, (float)ww, (float)wh);
    else drawMessages(a, d.dl, (float)ww, (float)wh);
    d.dl->PopTexture();
    d.dl->PopClipRect();
    d.dd.Valid = true;
    d.dd.DisplayPos = ImVec2(0, 0);
    d.dd.DisplaySize = ImVec2((float)ww, (float)wh);
    d.dd.FramebufferScale = ImVec2((float)fw / ww, (float)fh / wh);
    d.dd.Textures = &ImGui::GetPlatformIO().Textures;
    d.dd.AddDrawList(d.dl);
}

} // namespace

namespace detach {

const char* title(int k) { return k == Spectrum ? "OnAir spectrum" : k == Waterfall ? "OnAir waterfall" : "OnAir messages"; }

void frame(App& a) {
    for (int k = 0; k < kCount; k++) {
        Win& d = gWin[k];
        if (gNoOsWindows || !a.allowDetach) { a.detached[k] = false; closeOne(k); continue; }
        if (d.w && glfwWindowShouldClose(d.w)) a.detached[k] = false;   // its close button
        if (!a.detached[k]) { closeOne(k); continue; }
        if (!d.w && !openOne(k)) { a.detached[k] = false; a.engine.log(std::string("could not open the ") + title(k) + " window"); continue; }
        frameOne(a, k);
    }
}

void present() {
    for (Win& d : gWin) {
        if (!d.w || !d.surf || !d.dd.Valid) continue;
        int fw = 0, fh = 0;
        glfwGetFramebufferSize(d.w, &fw, &fh);
        d.surf->present(&d.dd, fw, fh);
    }
}

void shutdown() {
    for (int k = 0; k < kCount; k++) { closeOne(k); if (gWin[k].dl) { IM_DELETE(gWin[k].dl); gWin[k].dl = nullptr; } }
}

}  // namespace detach
