// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Airport weather table (acars_wx_ui.h): the newest METAR / SPECI and TAF per airport found in the datalink messages of the three modes.
#include "acars_wx_ui.h"
#include "app.h"
#include "dect2/acars/acars_metar.h"
#include <ctime>

namespace {
dect2::WxBook gBook;
uint64_t gLast[3] = {0, 0, 0};
const char* const kSrc[3] = {"ACARS", "VDL2", "HFDL"};
}

void acarsWxFeed(int source, uint64_t serial, const std::string& text, int64_t wallTime) {
    if (source < 0 || source > 2 || serial <= gLast[source] || text.empty()) return;
    gLast[source] = serial;
    gBook.addText(text, wallTime, kSrc[source]);
}

void acarsWxView() {
    { ImGui::PushTextWrapPos(0);
      ImGui::TextDisabled("%s", tr("Airport weather (METAR, SPECI, TAF) found in the text of ACARS, VDL2 and HFDL messages: weather requests, D-ATIS and printer uplinks. Flight category from ceiling and visibility (FAA: LIFR, IFR, MVFR, VFR)."));
      ImGui::PopTextWrapPos(); }
    const auto& rows = gBook.rows();
    if (rows.empty()) { ImGui::TextDisabled("%s", tr("no weather report in the messages yet")); return; }
    if (!ImGui::BeginTable("##acwx", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(1, 1);
    for (const char* c : {"Airport", "Kind", "Issued", "Cat", "Wind", "Vis", "Ceiling", "T / Td", "QNH"}) ImGui::TableSetupColumn(tr(c));
    ImGui::TableHeadersRow();
    for (const auto& [key, row] : rows) {
        const dect2::WxReport& r = row.rep;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(r.station.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s, heard %d times", r.raw.c_str(), row.source.c_str(), row.count);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(r.kind.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%02d %02d%02dZ", r.day, r.hour, r.minute);
        ImGui::TableNextColumn();
        const ImVec4 col = r.category == "LIFR" ? ImVec4(0.85f, 0.3f, 0.85f, 1) : r.category == "IFR" ? ImVec4(0.9f, 0.3f, 0.3f, 1) : r.category == "MVFR" ? ImVec4(0.35f, 0.55f, 0.95f, 1) : ImVec4(0.4f, 0.85f, 0.5f, 1);
        if (!r.category.empty()) ImGui::TextColored(col, "%s", r.category.c_str()); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        if (r.hasWind) { if (r.variable) ImGui::Text("VRB %d kt", r.windKt); else if (r.gustKt) ImGui::Text("%03d/%d G%d kt", r.windDir, r.windKt, r.gustKt); else ImGui::Text("%03d/%d kt", r.windDir, r.windKt); } else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (r.cavok) ImGui::TextUnformatted("CAVOK"); else if (r.hasVis) ImGui::Text("%.0f m", r.visM); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (r.hasCeiling) ImGui::Text("%d ft", r.ceilingFt); else ImGui::TextDisabled("%s", r.clouds.empty() ? "-" : r.clouds.c_str());
        ImGui::TableNextColumn(); if (r.hasTemp) ImGui::Text("%d / %d", r.tempC, r.dewC); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (r.hasQnh) ImGui::Text("%.0f", r.qnhHpa); else ImGui::TextDisabled("-");
    }
    ImGui::EndTable();
}
