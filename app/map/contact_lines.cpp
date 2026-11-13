// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Contact lines on the maps (contact_lines.h)
#include "app.h"
#include "../adsb/adsb_map.h"
#include "contact_lines.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace contactlines {
namespace {

Options O;
bool loaded = false;

// A colour for the band of a dial frequency (Hz); grey-blue when it is not known
ImU32 bandColour(double hz, int alpha) {
    static const ImU32 pal[12] = {IM_COL32(230, 90, 90, 0), IM_COL32(240, 140, 70, 0), IM_COL32(240, 190, 60, 0), IM_COL32(210, 220, 70, 0),
                                  IM_COL32(130, 220, 80, 0), IM_COL32(70, 210, 140, 0), IM_COL32(60, 210, 210, 0), IM_COL32(70, 160, 240, 0),
                                  IM_COL32(110, 110, 245, 0), IM_COL32(170, 100, 235, 0), IM_COL32(225, 100, 210, 0), IM_COL32(235, 120, 150, 0)};
    const double m = hz / 1e6;
    int i = -1;
    if (m > 0) i = m < 3 ? 0 : m < 4.5 ? 1 : m < 6 ? 2 : m < 8 ? 3 : m < 11 ? 4 : m < 15 ? 5 : m < 19 ? 6 : m < 22 ? 7 : m < 25.5 ? 8 : m < 30 ? 9 : m < 55 ? 10 : 11;
    const ImU32 c = i < 0 ? IM_COL32(150, 170, 200, 0) : pal[i];
    return (c & 0x00FFFFFF) | ((ImU32)alpha << 24);
}

}

Options& options() {
    if (!loaded) {
        loaded = true;
        auto& pr = plat::prefs();
        O.lines = pr.getB("ftxLines", true); O.fade = pr.getB("ftxLineFade", true); O.highlightDone = pr.getB("ftxLineDone", true);
        O.onlySel = pr.getB("ftxLineSel", false); O.labels = pr.getB("ftxLineLabels", false);
        O.colour = (int)pr.getI("ftxLineColour", 0);
        O.keepMin = (int)std::max(1L, std::min(120L, (long)pr.getI("ftxLineMin", 15)));
    }
    return O;
}

void controls() {
    Options& o = options();
    if (ImGui::Button(tr("contact lines"))) ImGui::OpenPopup("##contactlines");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A line between the two stations of a contact, drawn from the decoded messages: the great circle from the caller to the station called."));
    if (!ImGui::BeginPopup("##contactlines")) return;
    bool ch = false;
    ch |= ImGui::Checkbox(tr("show contact lines"), &o.lines);
    ch |= ImGui::Checkbox(tr("fade with age"), &o.fade);
    ch |= ImGui::Checkbox(tr("highlight completed contacts"), &o.highlightDone);
    ch |= ImGui::Checkbox(tr("only the selected station"), &o.onlySel);
    ch |= ImGui::Checkbox(tr("line labels"), &o.labels);
    ch |= ImGui::RadioButton(tr("colour by band"), &o.colour, 0);
    ImGui::SameLine();
    ch |= ImGui::RadioButton(tr("colour by time"), &o.colour, 1);
    ImGui::SetNextItemWidth(120 * gUi);
    ch |= ImGui::SliderInt(tr("keep lines for (min)"), &o.keepMin, 1, 120);
    if (!o.sel.empty()) {
        ImGui::TextDisabled("%s", o.sel.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("deselect"))) o.sel.clear();
    } else ImGui::TextDisabled("%s", tr("click a station on the map to select it"));
    if (ch) {
        auto& pr = plat::prefs();
        pr.setB("ftxLines", o.lines); pr.setB("ftxLineFade", o.fade); pr.setB("ftxLineDone", o.highlightDone); pr.setB("ftxLineSel", o.onlySel);
        pr.setB("ftxLineLabels", o.labels); pr.setI("ftxLineColour", o.colour); pr.setI("ftxLineMin", o.keepMin);
    }
    ImGui::EndPopup();
}

void pickAt(const dect2::ContactGraph& g, ImVec2 click) {
    Options& o = options();
    o.sel.clear();
    float best = 14 * gUi;
    for (const auto& s : g.stations()) {
        if (!s.known) continue;
        const ImVec2 q = adsbmap::project(s.lat, s.lon);
        const float d = std::hypot(q.x - click.x, q.y - click.y);
        if (d < best) { best = d; o.sel = s.call; }
    }
}

void markSelected(ImDrawList* dl, const std::string& call, ImVec2 q) {
    const Options& o = options();
    if (!o.sel.empty() && dect2::baseCallsign(call) == o.sel) dl->AddCircle(q, 8.f * gUi, IM_COL32(255, 255, 255, 230), 20, 2.f);
}

size_t draw(ImDrawList* dl, const dect2::ContactGraph& g, double now, double mapLon) {
    const Options& o = options();
    if (!o.lines) return 0;
    size_t n = 0;
    const double keep = o.keepMin * 60.0;
    for (const auto& e : g.edges()) {
        dect2::ContactStation sa, sb;
        if (!g.station(e.from, sa) || !g.station(e.to, sb) || !sa.known || !sb.known) continue;
        const bool sel = !o.sel.empty() && (e.from == o.sel || e.to == o.sel);
        if (o.onlySel && !sel) continue;
        const double age = std::max(0.0, now - e.last);
        const float k = o.fade ? (float)(1.0 - 0.75 * std::min(1.0, age / keep)) : 1.f;
        const int alpha = (int)(255 * k * 0.85f);
        ImU32 col;
        if (o.colour == 1) {   // time: new lines warm, old lines cold
            const float u = (float)std::min(1.0, age / keep);
            col = IM_COL32((int)(250 - 170 * u), (int)(190 - 20 * u), (int)(70 + 170 * u), alpha);
        } else col = bandColour(e.dialHz, alpha);
        float w = 1.4f * gUi;
        if (o.highlightDone && e.complete) { col = IM_COL32(110, 235, 130, std::max(alpha, 120)); w = 2.6f * gUi; }
        if (sel) w += 0.8f * gUi;
        const auto arc = dect2::greatCircleArc(sa.lat, sa.lon, sb.lat, sb.lon, 3.0);
        const double shift = std::round((mapLon - arc.front().second) / 360.0) * 360.0;
        std::vector<ImVec2> poly;
        poly.reserve(arc.size());
        for (const auto& pt : arc) poly.push_back(adsbmap::project(pt.first, pt.second + shift));
        dl->AddPolyline(poly.data(), (int)poly.size(), col, ImDrawFlags_None, w);
        n++;
        if (o.labels && poly.size() >= 2) {
            char lb[64];
            snprintf(lb, sizeof lb, "%s>%s", e.from.c_str(), e.to.c_str());
            const ImVec2 mid = poly[poly.size() / 2], ts = ImGui::CalcTextSize(lb);
            dl->AddRectFilled(ImVec2(mid.x - 2, mid.y - ts.y * 0.5f - 1), ImVec2(mid.x + ts.x + 2, mid.y + ts.y * 0.5f + 1), IM_COL32(10, 12, 14, 190), 2.f);
            dl->AddText(ImVec2(mid.x, mid.y - ts.y * 0.5f), col | 0xFF000000, lb);
        }
    }
    return n;
}

}
