// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "kit.h"
#include "../ui2_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace kit {

Tokens& tokens() { static Tokens t; return t; }

static ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) { return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.f); }

void syncTokens() {
    const ui2::UiTheme& P = ui2::T;
    Tokens& k = tokens();
    k.bg0 = P.bg; k.bg1 = P.rail; k.bg2 = P.button; k.bg3 = mix(P.button, P.bright, 0.10f);
    k.border = P.edge; k.text = P.text; k.textDim = P.dim; k.accent = P.accent;
    k.ok = ImVec4(0.42f, 0.75f, 0.45f, 1); k.warn = ImVec4(0.86f, 0.65f, 0.22f, 1); k.error = ImVec4(0.86f, 0.33f, 0.30f, 1);
    k.selection = mix(P.bg, P.accent, 0.22f); k.hover = mix(P.button, P.bright, 0.10f);
}

static ImU32 C(const ImVec4& c) { return u32(c); }

// ---------------------------------------------------------------- tabs
bool beginTabs(const char* id) {
    const Tokens& k = tokens();
    ImGui::PushStyleColor(ImGuiCol_Tab, k.bg1); ImGui::PushStyleColor(ImGuiCol_TabSelected, k.selection); ImGui::PushStyleColor(ImGuiCol_TabHovered, k.hover);
    ImGui::PushStyleColor(ImGuiCol_TabDimmed, k.bg1); ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, k.selection);
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, k.accent);
    const bool open = ImGui::BeginTabBar(id, ImGuiTabBarFlags_DrawSelectedOverline);
    ImGui::PopStyleColor(6);
    return open;
}
void endTabs() { ImGui::EndTabBar(); }
bool tab(const char* label, bool forceSelect) { return ImGui::BeginTabItem(label, nullptr, forceSelect ? ImGuiTabItemFlags_SetSelected : 0); }
void endTab() { ImGui::EndTabItem(); }

// ---------------------------------------------------------------- buttons
bool button(const char* label, bool primary) {
    const Tokens& k = tokens();
    if (primary) { ImGui::PushStyleColor(ImGuiCol_Button, mix(k.bg2, k.accent, 0.5f)); }
    const bool hit = ImGui::Button(label);
    if (primary) ImGui::PopStyleColor();
    return hit;
}
bool toggleButton(const char* label, bool* on) {
    const Tokens& k = tokens();
    if (*on) { ImGui::PushStyleColor(ImGuiCol_Button, mix(k.bg2, k.accent, 0.45f)); ImGui::PushStyleColor(ImGuiCol_Text, k.text); }
    const bool hit = ImGui::Button(label);
    if (*on) ImGui::PopStyleColor(2);
    if (hit) *on = !*on;
    return hit;
}
bool checkbox(const char* label, bool* on) { return ImGui::Checkbox(label, on); }
bool sw(const char* label, bool* on) {
    const Tokens& k = tokens();
    const float h = std::floor(ImGui::GetFrameHeight() * 0.8f), w = std::floor(h * 1.8f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    const bool hit = ImGui::InvisibleButton("##sw", ImVec2(w, ImGui::GetFrameHeight()));
    ImGui::PopID();
    if (hit) *on = !*on;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float y0 = p.y + (ImGui::GetFrameHeight() - h) * 0.5f;
    dl->AddRectFilled(ImVec2(p.x, y0), ImVec2(p.x + w, y0 + h), C(*on ? mix(k.bg2, k.accent, 0.6f) : k.bg2));
    dl->AddRect(ImVec2(p.x, y0), ImVec2(p.x + w, y0 + h), C(k.border));
    const float kx = *on ? p.x + w - h + 2 : p.x + 2;
    dl->AddRectFilled(ImVec2(kx, y0 + 2), ImVec2(kx + h - 4, y0 + h - 2), C(*on ? k.bg0 : k.textDim));
    ImGui::SameLine(0, px(k.s6));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    return hit;
}
bool iconButton(const char* id, Ic ic, const char* tip, bool active) {
    const Tokens& k = tokens();
    const float bs = std::floor(px(k.controlH + 2));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool hit = ImGui::InvisibleButton(id, ImVec2(bs, bs));
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hov) dl->AddRectFilled(p, ImVec2(p.x + bs, p.y + bs), C(k.hover));
    icons::draw(ic, ImVec2(p.x + bs * 0.5f, p.y + bs * 0.5f), bs * 0.62f, C(active ? k.accent : hov ? k.text : k.textDim), dl);
    if (hov && tip) ImGui::SetTooltip("%s", tip);
    return hit;
}
bool sliderValue(const char* label, float* v, float lo, float hi, const char* fmt, float width) {
    if (width > 0) ImGui::SetNextItemWidth(width);
    return ImGui::SliderFloat(label, v, lo, hi, fmt);
}
bool combo(const char* label, int* cur, const char* const* items, int n, float width) {
    if (width > 0) ImGui::SetNextItemWidth(width);
    bool changed = false;
    if (ImGui::BeginCombo(label, items[std::max(0, std::min(n - 1, *cur))])) {
        for (int i = 0; i < n; i++) if (ImGui::Selectable(items[i], *cur == i)) { *cur = i; changed = true; }
        ImGui::EndCombo();
    }
    return changed;
}

// ---------------------------------------------------------------- containers
bool panelHeader(const char* title, bool* open, const char* rightText) {
    const Tokens& k = tokens();
    const float hh = std::floor(px(k.headerH));
    const ImVec2 w = ImGui::GetContentRegionAvail();
    const ImVec2 h0 = ImGui::GetCursorScreenPos();
    ImGui::PushID(title);
    const bool hit = ImGui::InvisibleButton("##hdr", ImVec2(w.x, hh));
    ImGui::PopID();
    if (hit && open) *open = !*open;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = h0.y + hh * 0.5f, ax = h0.x + px(k.s8), r = px(3.5f);
    if (open) {
        if (!*open) dl->AddTriangleFilled(ImVec2(ax - r * 0.6f, cy - r), ImVec2(ax - r * 0.6f, cy + r), ImVec2(ax + r, cy), C(k.textDim));
        else dl->AddTriangleFilled(ImVec2(ax - r, cy - r * 0.6f), ImVec2(ax + r, cy - r * 0.6f), ImVec2(ax, cy + r), C(k.textDim));
    }
    const float ty = h0.y + (hh - ImGui::GetTextLineHeight()) * 0.5f;
    dl->AddText(ImVec2(h0.x + (open ? px(k.s16 + 2) : px(k.s8)), ty), C(k.text), title);
    if (rightText) dl->AddText(ImVec2(h0.x + w.x - ImGui::CalcTextSize(rightText).x - px(k.s8), ty), C(k.textDim), rightText);
    dl->AddLine(ImVec2(h0.x, h0.y + hh - 1), ImVec2(h0.x + w.x, h0.y + hh - 1), C(k.border), 1.f);
    return hit;
}
void sectionHeader(const char* text) {
    const Tokens& k = tokens();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", text);
    const float x0 = p.x + ImGui::CalcTextSize(text).x + px(k.s6), x1 = p.x + ImGui::GetContentRegionAvail().x, y = p.y + ImGui::GetFrameHeight() * 0.5f;
    if (x1 > x0 + px(8)) ImGui::GetWindowDrawList()->AddLine(ImVec2(x0, y), ImVec2(x1, y), C(k.border), 1.f);
}
bool collapsingSection(const char* title, bool openByDefault) {
    const Tokens& k = tokens();
    ImGui::PushStyleColor(ImGuiCol_Header, k.bg1); ImGui::PushStyleColor(ImGuiCol_HeaderHovered, k.hover);
    const bool open = ImGui::CollapsingHeader(title, openByDefault ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    ImGui::PopStyleColor(2);
    return open;
}

// a flow row: the caller states each item's width before drawing it; an item that does not fit starts the next line
static struct { float x0 = 0, right = 0; bool first = true; } gFlow;
void beginFlow(float rightReserve) {
    gFlow.x0 = ImGui::GetCursorScreenPos().x;
    gFlow.right = gFlow.x0 + ImGui::GetContentRegionAvail().x - rightReserve;
    gFlow.first = true;
}
void flowItem(float width) {
    const Tokens& k = tokens();
    if (!gFlow.first) {
        const float x = ImGui::GetItemRectMax().x + px(k.s6);
        if (x + width <= gFlow.right) ImGui::SameLine(0, px(k.s6));
    }
    gFlow.first = false;
}
void flowRight(float width) {
    const float x = gFlow.right - width;
    if (x > ImGui::GetItemRectMax().x + px(tokens().s6)) { ImGui::SameLine(); ImGui::SetCursorScreenPos(ImVec2(x, ImGui::GetItemRectMin().y)); }
    gFlow.first = false;
}
void endFlow() {}

bool beginTable(const char* id, int columns, const char* const* headers) {
    const Tokens& k = tokens();
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(px(k.s4), std::max(0.f, (px(k.rowH) - ImGui::GetTextLineHeight()) * 0.5f)));
    const bool open = ImGui::BeginTable(id, columns, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp);
    ImGui::PopStyleVar();
    if (open && headers) {
        for (int i = 0; i < columns; i++) ImGui::TableSetupColumn(headers[i]);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
    }
    return open;
}
void endTable() { ImGui::EndTable(); }

void statusBarBegin(float width) {
    const Tokens& k = tokens();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, k.bg1);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(10), px(k.s4)));
    ImGui::BeginChild("##statusbar", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::GetWindowDrawList()->AddLine(ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + width, ImGui::GetWindowPos().y), C(k.border));
}
void statusBarEnd() { ImGui::EndChild(); ImGui::PopStyleColor(); }

void tooltip(const char* text, const char* keyHint) {
    if (!ImGui::IsItemHovered()) return;
    if (keyHint) ImGui::SetTooltip("%s  [%s]", text, keyHint); else ImGui::SetTooltip("%s", text);
}

void meter(float frac, float width, float warnAt, float errorAt) {
    const Tokens& k = tokens();
    frac = std::min(1.f, std::max(0.f, frac));
    const float h = px(k.s8);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), C(k.bg2));
    const ImVec4& col = frac >= errorAt ? k.error : frac >= warnAt ? k.warn : k.ok;
    dl->AddRectFilled(p, ImVec2(p.x + width * frac, p.y + h), C(col));
    dl->AddRect(p, ImVec2(p.x + width, p.y + h), C(k.border));
}
void labelValue(const char* label, const char* fmt, ...) {
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(0, tokens().s6 * gUi);
    char b[256];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::TextUnformatted(b);
}

}  // namespace kit
