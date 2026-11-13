// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Layout helpers of the side panels (Source, Gain, Tuner): small sub-headings, one label column for every row, a record button with its dot
// drawn in a fixed place, and a compact warning chip. Every radio's gain rows use the same helpers, so the panels line up in the Current,
// Clean and Compact looks and at any UI scale (all sizes follow the font and gUi).
#pragma once
#include "imgui.h"
#include "theme.h"
#include "kit/kit.h"
#include <algorithm>
#include <string>

#include "scale.h"
#include <cmath>

namespace panel {

// the width of the label column: the longest label of a row in the panels ("Auto gain", "DC spike", "Bias-tee")
inline float labelW() { return ImGui::CalcTextSize("Auto gain").x + 10.f * gUi; }

// a small heading with a thin rule to the right: "Level", "Corrections", "Antenna power", "Radio", "Record", "Listen"
// where the first row below an open collapsible section's header starts (set by section()): a sub-heading there needs no gap above it
inline float gSectionTopY = -1.f;
inline int gSectionTopFrame = -1;

inline void subHeading(const char* text) {
    const bool first = gSectionTopFrame == ImGui::GetFrameCount() && std::fabs(ImGui::GetCursorPosY() - gSectionTopY) < 1.f;
    if (!first) ImGui::Dummy(ImVec2(0, 2 * gUi));
    if (pal::dev()) { kit::sectionHeader(text); return; }
    ImGui::AlignTextToFramePadding();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::TextDisabled("%s", text);
    const float x0 = p.x + ImGui::CalcTextSize(text).x + 6 * gUi, x1 = p.x + ImGui::GetContentRegionAvail().x, y = p.y + ImGui::GetFrameHeight() * 0.5f;
    if (x1 > x0 + 8 * gUi) ImGui::GetWindowDrawList()->AddLine(ImVec2(x0, y), ImVec2(x1, y), ImGui::GetColorU32(ImGuiCol_Separator), 1.f);
}

// a row: the label in the label column (with its tooltip), then the control starts at the column; returns the width left for the control
inline float rowLabel(const char* label, const char* tip = nullptr) {
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", label);
    if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip);
    ImGui::SameLine(x0 + labelW());
    return std::max(60.f * gUi, ImGui::GetContentRegionAvail().x);
}

// the compact warning mark: an amber triangle with an exclamation mark, as tall as a text line
inline void warnIcon() {
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::GetColorU32(pal::warnAmber());
    const float w = h * 1.05f, t = p.y + h * 0.10f, b = p.y + h * 0.92f;
    dl->AddTriangleFilled(ImVec2(p.x + w * 0.5f, t), ImVec2(p.x, b), ImVec2(p.x + w, b), col);
    const ImU32 ink = IM_COL32(30, 24, 8, 255);
    dl->AddRectFilled(ImVec2(p.x + w * 0.5f - 0.7f * gUi, t + h * 0.28f), ImVec2(p.x + w * 0.5f + 0.7f * gUi, t + h * 0.55f), ink);
    dl->AddRectFilled(ImVec2(p.x + w * 0.5f - 0.7f * gUi, t + h * 0.62f), ImVec2(p.x + w * 0.5f + 0.7f * gUi, t + h * 0.74f), ink);
    ImGui::Dummy(ImVec2(w, h));
}

// the width a record button needs: its dot, a gap, its text and the button padding
inline float dotW() { return 6.4f * gUi + 5.f * gUi; }
inline float dotPadW() { const float sp = std::max(1.f, ImGui::CalcTextSize(" ").x); return std::ceil(dotW() / sp) * sp; }   // the spaces in front of the text
inline float dotButtonW(const char* label) { return dotPadW() + ImGui::CalcTextSize(label).x + 2.f * ImGui::GetStyle().FramePadding.x; }

// a small button with a red record dot at its left edge: the dot sits one padding in, the text after it, in every record row
inline bool dotButton(const char* label, bool enabled) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float lead = dotW();
    const float sp = std::max(1.f, ImGui::CalcTextSize(" ").x);
    std::string pad((size_t)std::ceil(lead / sp), ' ');
    const bool hit = ImGui::SmallButton((pad + label).c_str());
    const ImVec2 sz = ImGui::GetItemRectSize();
    const float r = 3.2f * gUi;
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p0.x + st.FramePadding.x + r, p0.y + sz.y * 0.5f), r,
        enabled ? ImGui::GetColorU32(pal::badRed()) : ImGui::GetColorU32(ImGuiCol_TextDisabled));
    return hit;
}

}  // namespace panel
