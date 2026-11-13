// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Ham view's world map (decoded stations, great-circle lines, the grayline, distance and bearing) and the spots panel (every station
// heard, from the decodes; nothing is fetched from the network): see ham_ops_ui.h.
#include "ham_ops_ui.h"
#include "../adsb/adsb_map.h"
#include "dect2/geo/maidenhead.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>

using namespace dect2;

namespace {
struct MapUi {
    bool init = false, gray = true, longPath = false, labels = true, cqOnly = false;
    adsbmap::View view;
};
MapUi M;

void line(ImDrawList* dl, const std::vector<std::pair<double, double>>& pts, ImU32 col, float w, float width) {
    ImVec2 prev; bool have = false;
    for (const auto& p : pts) {
        const ImVec2 q = adsbmap::project(p.first, p.second);
        if (have && std::fabs(q.x - prev.x) < width * 0.5f) dl->AddLine(prev, q, col, w);
        prev = q; have = true;
    }
}
bool myPos(double& la, double& lo) {
    if (fromMaidenhead(hamOps().myGrid, la, lo)) return true;
    receiverLocation(la, lo);
    return receiverLocationSet();
}
ImU32 spotColour(const ham::Spot& s, const ham::DecodeMarks& m) {
    if (m.newDxcc) return IM_COL32(230, 80, 230, 255);
    if (m.newDxccBand) return IM_COL32(240, 150, 230, 255);
    if (m.newGrid) return IM_COL32(90, 200, 250, 255);
    if (m.workedCallBand) return IM_COL32(150, 150, 150, 220);
    return s.cq ? IM_COL32(90, 220, 110, 255) : IM_COL32(240, 220, 120, 255);
}
}  // namespace

void hamMapTab(App& a) {
    (void)a;
    HamOps& o = hamOps();
    double la = 0, lo = 0;
    const bool haveMe = myPos(la, lo);
    if (!M.init) { M.init = true; M.view.lat = haveMe ? la : 30; M.view.lon = haveMe ? lo : 0; M.view.zoom = 2; }
    const double now = (double)time(nullptr);
    ImGui::Checkbox(tr("Grayline"), &M.gray); ImGui::SameLine();
    ImGui::Checkbox(tr("Long path"), &M.longPath); ImGui::SameLine();
    ImGui::Checkbox(tr("Labels"), &M.labels); ImGui::SameLine();
    ImGui::Checkbox(tr("CQ only"), &M.cqOnly); ImGui::SameLine();
    double sl, so;
    ham::subsolarPoint(now, sl, so);
    if (haveMe) {
        const double el = ham::sunElevation(la, lo, now);
        ImGui::TextDisabled("%s %+.0f deg%s", tr("sun at my station"), el, std::fabs(el) <= 6 ? tr(", in the grayline now") : "");
    }
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, std::max(160 * gUi, ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing() * 2));
    adsbmap::draw(M.view, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    if (M.gray) {   // night shading in 2 degree strips, the terminator, the sun
        const auto t = ham::terminator(now, 181);
        for (size_t i = 0; i + 1 < t.size(); i++) {
            const double tl = t[i].first, l0 = t[i].second, l1 = t[i + 1].second;
            const bool nightNorth = ham::sunElevation(std::min(84.0, tl + 1), l0, now) < 0;
            const ImVec2 q0 = adsbmap::project(nightNorth ? 85 : tl, l0), q1 = adsbmap::project(nightNorth ? tl : -85, l1);
            if (std::fabs(q1.x - q0.x) > size.x * 0.5f) continue;   // across the edge of the world
            dl->AddRectFilled(ImVec2(std::min(q0.x, q1.x), std::min(q0.y, q1.y)), ImVec2(std::max(q0.x, q1.x) + 0.5f, std::max(q0.y, q1.y)), IM_COL32(0, 0, 30, 70));
        }
        line(dl, t, IM_COL32(255, 200, 80, 170), 1.5f * gUi, size.x);
        const ImVec2 s = adsbmap::project(sl, so);
        dl->AddCircleFilled(s, 6 * gUi, IM_COL32(255, 210, 60, 230));
    }
    // the stations heard (with a grid), coloured as the decode list
    struct Hit { ImVec2 at; const ham::Spot* s; };
    std::vector<Hit> hits;
    for (const auto& s : o.spots.spots()) {
        if (s.grid.empty() || (M.cqOnly && !s.cq)) continue;
        double sla, slo;
        if (!fromMaidenhead(s.grid, sla, slo)) continue;
        const ImVec2 q = adsbmap::project(sla, slo);
        const ham::DecodeMarks m = o.worked.mark(s.call, s.grid, s.band, s.mode, hamDxccKey(s.call));
        const ImU32 c = spotColour(s, m);
        dl->AddCircleFilled(q, 3.5f * gUi, c);
        if (M.labels && M.view.zoom >= 3) dl->AddText(ImVec2(q.x + 5 * gUi, q.y - 7 * gUi), c, s.call.c_str());
        hits.push_back({q, &s});
    }
    // lines: the running QSO (red), the picked station (white)
    auto pathTo = [&](const std::string& grid, ImU32 col, const std::string& call) {
        double tla, tlo;
        if (!haveMe || !fromMaidenhead(grid, tla, tlo)) return;
        line(dl, ham::gcPath(la, lo, tla, tlo, 96, false), col, 2 * gUi, size.x);
        if (M.longPath) line(dl, ham::gcPath(la, lo, tla, tlo, 160, true), (col & 0x00FFFFFF) | 0x60000000, 1.5f * gUi, size.x);
        double km, brg;
        greatCircle(la, lo, tla, tlo, km, brg);
        const ImVec2 q = adsbmap::project(tla, tlo);
        char b[96]; snprintf(b, sizeof b, "%s  %.0f km  %.0f deg", call.c_str(), km, brg);
        dl->AddCircle(q, 7 * gUi, col, 16, 2 * gUi);
        dl->AddText(ImVec2(q.x + 9 * gUi, q.y + 3 * gUi), col, b);
    };
    if (!o.selGrid.empty() && o.selCall != o.dxCall) pathTo(o.selGrid, IM_COL32(255, 255, 255, 230), o.selCall);
    if (!o.dxGrid.empty()) pathTo(o.dxGrid, IM_COL32(255, 90, 80, 240), o.dxCall);
    dl->PopClipRect();
    // hover and click on a station
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const Hit* best = nullptr;
    float bd = 8 * gUi;
    for (const auto& h : hits) { const float d = std::hypot(h.at.x - mp.x, h.at.y - mp.y); if (d < bd) { bd = d; best = &h; } }
    if (best && ImGui::IsWindowHovered()) {
        const ham::Spot& s = *best->s;
        ham::PathInfo pi;
        const bool path = ham::pathBetween(o.myGrid, s.grid, pi);
        ImGui::SetTooltip("%s  %s\n%s  %s %d dB  %s\n%s", s.call.c_str(), hamCountry(s.call).c_str(), s.grid.c_str(), s.band.c_str(), s.snr, s.mode.c_str(),
                          path ? (std::to_string((int)pi.km) + " km, " + std::to_string((int)pi.bearing) + " deg").c_str() : "");
        ImVec2 cl;
        if (adsbmap::clickedAt(cl)) { o.selCall = s.call; o.selGrid = s.grid; }
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) M.view.zoom = std::min(adsbmap::kMaxZoom, M.view.zoom + 1);
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) M.view.zoom = std::max(adsbmap::kMinZoom, M.view.zoom - 1);
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("me") && haveMe) { M.view.lat = la; M.view.lon = lo; }
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the stations or your grid");
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
    adsbmap::legend(size.x, "magenta new DXCC, pink new on band, blue new grid, green CQ, grey worked; red line: the QSO, white: the picked station");
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
    ImGui::Dummy(ImVec2(0, 0));
    if (!haveMe) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", tr("Enter your grid locator for the paths, distances and bearings."));
}

void hamSpotsTab(App& a, const std::function<void(const std::string&, const std::string&)>& callIt) {
    (void)a;
    HamOps& o = hamOps();
    static bool cqOnly = false, newOnly = false, bandOnly = false;
    static char filt[24] = {};
    const double now = (double)time(nullptr);
    o.spots.expire(now, 3600);
    ImGui::Checkbox(tr("CQ only"), &cqOnly); ImGui::SameLine();
    ImGui::Checkbox(tr("New only"), &newOnly); ImGui::SameLine();
    ImGui::Checkbox(tr("This band"), &bandOnly); ImGui::SameLine();
    ImGui::SetNextItemWidth(120 * gUi);
    ImGui::InputTextWithHint("##spf", tr("call or country"), filt, sizeof filt, ImGuiInputTextFlags_CharsUppercase);
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Clear"))) o.spots.clear();
    ImGui::TextDisabled("%s", tr("Stations heard in the last hour (from your own decodes). Double click: call the station."));
    const std::string band = ham::bandNameFor(a.freqMhz * 1e6);
    const ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("##spots", 10, fl, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(1, 1);
    for (const char* c : {"Call", "Country", "Band", "Mode", "dB", "Best", "Grid", "km", "deg", "Last"}) ImGui::TableSetupColumn(c, !std::string(c).compare("Last") ? ImGuiTableColumnFlags_WidthStretch : 0);
    ImGui::TableHeadersRow();
    int row = 0;
    for (const auto& s : o.spots.spots()) {
        if (cqOnly && !s.cq) continue;
        if (bandOnly && s.band != band) continue;
        const std::string country = hamCountry(s.call);
        if (filt[0]) { std::string cu = country; for (char& ch : cu) ch = (char)toupper((unsigned char)ch); if (s.call.find(filt) == std::string::npos && cu.find(filt) == std::string::npos) continue; }
        const ham::DecodeMarks m = o.worked.mark(s.call, s.grid, s.band, s.mode, hamDxccKey(s.call));
        if (newOnly && m.workedCallBand && !m.newDxccBand && !m.newGridBand) continue;   // new: not worked on this band and mode, or a new entity / grid
        ImGui::TableNextRow();
        ImGui::PushID(row++);
        ImGui::TableNextColumn();
        const ImU32 c = spotColour(s, m);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(c));
        if (ImGui::Selectable(s.call.c_str(), o.selCall == s.call, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
            o.selCall = s.call; o.selGrid = s.grid;
            if (ImGui::IsMouseDoubleClicked(0) && callIt) callIt(s.call, s.grid);
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && !s.lastMsg.empty()) ImGui::SetTooltip("%s", s.lastMsg.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(country.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(s.band.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(s.mode.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%+d", s.snr);
        ImGui::TableNextColumn(); ImGui::Text("%+d", s.bestSnr);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(s.grid.c_str());
        ham::PathInfo pi;
        const bool p = ham::pathBetween(o.myGrid, s.grid, pi);
        ImGui::TableNextColumn(); if (p) ImGui::Text("%.0f", pi.km);
        ImGui::TableNextColumn(); if (p) ImGui::Text("%.0f", pi.bearing);
        ImGui::TableNextColumn(); ImGui::Text("%.0f min ago, %dx", (now - s.lastUtc) / 60, s.count);
        ImGui::PopID();
    }
    ImGui::EndTable();
}
