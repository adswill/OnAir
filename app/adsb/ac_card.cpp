// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "ac_card.h"
#include "app.h"
#include "map/my_location.h"
#include "i18n/i18n.h"
#include "ui/scale.h"
#include <algorithm>
#include <cstdio>

namespace adsbmap {

namespace {
void row(const char* k, const std::string& v) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k);
    ImGui::TableNextColumn(); ImGui::TextUnformatted(v.c_str());
}
}

void aircraftCard(const char* id, ImVec2 p0, ImVec2 size, ImVec2 q, const AcCard& c) {
    std::string reg = c.reg;
    while (!reg.empty() && reg[0] == '.') reg.erase(0, 1);   // ACARS pads registrations with dots
    if (reg.empty() && c.icao) reg = adsbRegOfIcao(c.icao);
    const std::string type = c.icao ? adsbTypeOfIcao(c.icao) : std::string();
    char hex[16] = "";
    if (c.icao) snprintf(hex, sizeof hex, "%06X", c.icao);

    static ImVec2 cardSize(200 * gUi, 150 * gUi);
    const float margin = 4 * gUi;
    float px = q.x + 20 * gUi, py = q.y + 12 * gUi;
    if (px + cardSize.x > p0.x + size.x - margin) px = q.x - 20 * gUi - cardSize.x;
    if (py + cardSize.y > p0.y + size.y - margin) py = q.y - 12 * gUi - cardSize.y;
    px = std::clamp(px, p0.x + margin, std::max(p0.x + margin, p0.x + size.x - margin - cardSize.x));
    py = std::clamp(py, p0.y + margin, std::max(p0.y + margin, p0.y + size.y - margin - cardSize.y));
    ImGui::SetNextWindowPos(ImVec2(px, py), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(std::max(1.f, size.x - 2 * margin), std::max(1.f, size.y - 2 * margin)));
    ImGui::SetNextWindowBgAlpha(0.94f);
    if (!ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::End();
        return;
    }
    const std::string title = !reg.empty() ? reg : !c.flight.empty() ? c.flight : hex[0] ? std::string(hex) : std::string("?");
    ImGui::TextColored(ImVec4(1, 1, 1, 1), "%s", title.c_str());
    ImGui::Separator();
    if (ImGui::BeginTable("##accardg", 2, ImGuiTableFlags_SizingFixedFit)) {
        row(tr("Registration"), reg.empty() ? "-" : reg);
        row(tr("Type"), !type.empty() ? type : c.icao ? std::string("-") : std::string(tr("needs the ICAO address")));
        if (!c.flight.empty()) row(tr("Flight"), c.flight);
        if (c.icao) row("ICAO", hex);
        if (c.hasAlt) { char b[40]; snprintf(b, sizeof b, "FL%03d  (%d ft)", (c.altFt + 50) / 100, c.altFt); row(tr("Altitude"), b); }
        std::string from;
        row(tr("From me"), mylocmap::describeFromMe(c.lat, c.lon, from) ? from : std::string(tr("set my location (View > Map)")));
        ImGui::EndTable();
    }
    if (!c.source.empty()) ImGui::TextDisabled("%s", c.source.c_str());
    cardSize = ImGui::GetWindowSize();
    ImGui::End();
}

}
