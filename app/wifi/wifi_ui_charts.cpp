// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi channels screen: a per-band summary (networks, standards, security) and the channel occupancy chart of the chosen band: networks per
// channel as bars, the strongest level on top, so the quietest channel of a band is easy to pick (see wifi_ui_int.h).
#include "wifi_ui_int.h"
#include "dect2/wifi/wifi_chan.h"
#include "dect2/wifi/wifi_proto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace wfui {

// The newest generation in "a/n/ac/ax": the text after the last slash.
const char* stdWord(const std::string& s) {
    const size_t p = s.rfind('/');
    return s.c_str() + (p == std::string::npos ? 0 : p + 1);
}

// One row per band: how many networks, how many Wi-Fi 6 / 7, WPA3, open, and the strongest level.
static void summary(const dect2::WifiTelemetry& t) {
    if (!ImGui::BeginTable("##wfsum", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableSetupColumn(tr("Band")); ImGui::TableSetupColumn(tr("Networks")); ImGui::TableSetupColumn(tr("Beacons")); ImGui::TableSetupColumn("ax / be");
    ImGui::TableSetupColumn("WPA3 / OWE"); ImGui::TableSetupColumn(tr("Open")); ImGui::TableSetupColumn(tr("Strongest"));
    ImGui::TableHeadersRow();
    for (int b = 0; b < 3; b++) {
        int n = 0, ax = 0, w3 = 0, open = 0;
        uint64_t beacons = 0;
        float best = -200;
        for (const auto& d : t.aps) {
            if (d.band != b) continue;
            n++; beacons += d.beacons;
            if (d.standard.find("ax") != std::string::npos || d.standard.find("be") != std::string::npos) ax++;
            if (d.security == dect2::wifi::Wpa3 || d.security == dect2::wifi::Wpa2Wpa3 || d.security == dect2::wifi::Owe) w3++;
            if (d.security == dect2::wifi::Open) open++;
            best = std::max(best, d.rssi);
        }
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::wifi::bandName(b));
        ImGui::TableNextColumn(); ImGui::Text("%d", n);
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)beacons);
        ImGui::TableNextColumn(); ImGui::Text("%d", ax);
        ImGui::TableNextColumn(); ImGui::Text("%d", w3);
        ImGui::TableNextColumn(); ImGui::Text("%d", open);
        ImGui::TableNextColumn(); if (n) ImGui::Text("%.0f dBFS", best); else ImGui::TextDisabled("-");
    }
    ImGui::EndTable();
}

// Bars: networks per channel of the selected band; the number above a bar is the strongest level (dBFS).
static void occupancy(const dect2::WifiTelemetry& t) {
    using namespace dect2::wifi;
    const std::vector<int> chans = channelList(S.band, S.domain);
    if (chans.empty()) return;
    std::map<int, int> count;
    std::map<int, float> best;
    int peak = 1;
    for (const auto& d : t.aps) {
        if (d.band != S.band || !d.channel) continue;
        const int c = d.channel;
        count[c]++;
        best[c] = best.count(c) ? std::max(best[c], d.rssi) : d.rssi;
        peak = std::max(peak, count[c]);
    }
    ImGui::TextDisabled("%s", tr("Networks per channel"));
    const float h = 150 * gUi, labelH = ImGui::GetTextLineHeight() * 2 + 4;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = std::max(60.0f, ImGui::GetContentRegionAvail().x);
    ImGui::Dummy(ImVec2(w, h + labelH));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float bw = w / (float)chans.size();
    const ImU32 txt = ImGui::GetColorU32(ImGuiCol_Text), dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h), ImGui::GetColorU32(ImGuiCol_Border));
    for (size_t i = 0; i < chans.size(); i++) {
        const int c = chans[i];
        const int n = count.count(c) ? count[c] : 0;
        const float x0 = p0.x + bw * (float)i, x1 = x0 + std::max(1.0f, bw - 2);
        if (n) {
            const float y = p0.y + h - (h - 18 * gUi) * (float)n / (float)peak;
            const bool dfs = S.band == Band5 && isDfs(c);
            dl->AddRectFilled(ImVec2(x0 + 1, y), ImVec2(x1, p0.y + h - 1), dfs ? IM_COL32(200, 150, 60, 220) : IM_COL32(70, 160, 190, 220));
            char b[16];
            snprintf(b, sizeof b, "%.0f", best[c]);
            if (bw > 22 * gUi) dl->AddText(ImVec2(x0 + 2, y - ImGui::GetTextLineHeight()), dim, b);
        }
        if (bw > 14 * gUi || i % 2 == 0) {
            char b[8];
            snprintf(b, sizeof b, "%d", c);
            const ImVec2 sz = ImGui::CalcTextSize(b);
            if (bw >= sz.x + 2 || i % 3 == 0) dl->AddText(ImVec2(x0 + (bw - sz.x) / 2, p0.y + h + 2), (S.band == Band6 && isPsc(c)) ? txt : dim, b);
        }
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", S.band == Band24 ? tr("Channels 1, 6 and 11 do not overlap.")
                          : S.band == Band5 ? tr("Amber: DFS channels (radar sharing).")
                                            : tr("Bright numbers: preferred scanning channels (PSC), where FILS discovery is sent."));
    ImGui::PopTextWrapPos();
}

// The Channels sub-view.
void channelsView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to see the channels")); return; }
    const dect2::WifiTelemetry& t = a.rx.wifi;
    summary(t);
    ImGui::Spacing();
    occupancy(t);
}

} // namespace wfui
