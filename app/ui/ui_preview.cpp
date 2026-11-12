// The hover preview of the appearance choices: the tooltip, the settings in use, the dev gallery (ui_preview.h, docs/UI-PREVIEW-4.0.md).
// The miniature itself is drawn by ui_preview_draw.cpp.
#include "ui_preview.h"
#include "../app.h"
#include "../i18n/i18n.h"
#include "../platform/platform.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "kit/mode_panel.h"
#include "scale.h"
#include <algorithm>
#include <cmath>

namespace uiprev {

bool& enabled() {
    static bool on = [] { return plat::prefs().getB("uiPreviews", true); }();
    return on;
}

Look lookOf(const App& a) {
    Look l;
    l.newUi = a.newUi; l.theme = a.uiTheme; l.mode = a.highContrast ? 2 : a.lightUi ? 1 : 0; l.variant = a.uiVariant; l.unified = a.layoutUnified;
    l.typography = (int)plat::prefs().getI("uiTypography", 0);   // the saved choice (the fonts follow it at the next start)
    l.textScale = a.uiTextScale; l.touch = a.touchUi; l.touchScale = a.touchScale; l.easy = modepanel::easyModeOn();
    l.wfMap = a.wf.map; l.tunerStyle = a.tunerStyle; l.traceColour = a.traceColor;
    l.accentOn = ui2::userAccent(l.accent);
    return l;
}

void item(const App& a, Opt o, double v, const char* note) {
    if (!enabled() || a.touchUi) return;   // a touch screen has no hover
    ImGuiContext& g = *GImGui;
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    const bool focused = ImGui::IsItemFocused() && g.NavCursorVisible;   // the keyboard went to this item
    if (!hovered && !focused) return;
    const Look cur = lookOf(a);
    const Desc d = previewFor(cur, o, v);
    const Desc now = describe(cur);
    // the miniature keeps its size at every display scale; inside, the look's own text size and touch scale act on it
    const float w = previewW(d) * gUi, h = previewH(d) * gUi, unit = unitFor(now, d, gUi);
    if (!ImGui::BeginTooltip()) return;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    draw(ImGui::GetWindowDrawList(), p.x, p.y, w, h, d, unit, frameTurned(cur));
    ImGui::Dummy(ImVec2(w, h));
    if (note && *note) ImGui::TextDisabled("%s", note);
    ImGui::EndTooltip();
}

bool& galleryOn() { static bool on = false; return on; }

void gallery(const App& a, float dispW, float dispH) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(dispW, dispH));
    ImGui::Begin("##previewgallery", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    const Look cur = lookOf(a);
    const Desc now = describe(cur);
    // every picture fits on the page: the cell size follows the window
    const float gap = 8 * gUi, lineH = ImGui::GetTextLineHeightWithSpacing();
    int cols = 1;
    float k = 1.f;
    for (int c = 1; c <= kGalleryCount; c++) {
        const int rows = (kGalleryCount + c - 1) / c;
        const float kw = (dispW - gap * (c + 1)) / (c * 260.f * gUi), kh = (dispH - gap - rows * (lineH + gap)) / (rows * 160.f * gUi);
        if (std::min(kw, kh) > k || c == 1) { k = std::min(kw, kh); cols = c; }
    }
    k = std::max(0.2f, std::min(1.f, k));
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < kGalleryCount; i++) {
        const Sample& s = kGallery[i];
        const Desc d = previewFor(cur, s.o, s.v);
        const float cw = 260.f * gUi * k, ch = 160.f * gUi * k;
        const float x = o.x + (i % cols) * (cw + gap), y = o.y + (i / cols) * (ch + lineH + gap);
        dl->AddText(ImVec2(x, y), ImGui::GetColorU32(ImGuiCol_Text), s.name);
        draw(dl, x, y + lineH, previewW(d) * gUi * k, std::min(ch, previewH(d) * gUi * k), d, unitFor(now, d, gUi) * k, frameTurned(cur));
    }
    ImGui::Dummy(ImVec2(1, 1));
    ImGui::End();
}

}   // namespace uiprev
