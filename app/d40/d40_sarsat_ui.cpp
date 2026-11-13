// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cospas-Sarsat: beacons grouped into incidents (repeat interval, self-tests kept apart, position drift). docs/WIRING-4.0.md
#include "d40_ui.h"
#include "../ui/typography.h"
#include "dect2/sarsat/sarsat_incidents.h"
#include <cstdio>

namespace {
dect2::SarsatIncidents inc;
int64_t lastWall = 0;
double lastTime = 0;
}  // namespace

void d40SarsatFeed(const dect2::SarsatTelemetry& t) {
    if (t.log.empty()) return;
    if (t.timeSec < lastTime) { inc = dect2::SarsatIncidents(); lastWall = 0; }   // receiver restarted
    lastTime = t.timeSec;
    int64_t newest = lastWall;
    for (auto it = t.log.rbegin(); it != t.log.rend(); ++it) {   // oldest first
        if (it->wallTime <= lastWall) continue;
        inc.add(it->b, (double)it->wallTime);
        newest = std::max(newest, it->wallTime);
    }
    lastWall = newest;
}

void d40SarsatIncidentsCard() {
    if (!typo::collapsingHeader(trw("Incidents (one row per beacon)"), 0)) return;
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Bursts of the same beacon ID grouped. A real beacon repeats about every 50 seconds and, if it carries a position, sends it again; a 5 second echo or a single burst is not flagged. Self-test bursts are counted separately. Receive only: do not rely on this for safety."));
    const auto l = inc.list();
    if (l.empty()) { ImGui::TextDisabled("%s", tr("No beacon yet.")); return; }
    if (ImGui::BeginTable("##d40sari", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(tr("Hex ID")); ImGui::TableSetupColumn(tr("Type")); ImGui::TableSetupColumn(tr("Bursts"));
        ImGui::TableSetupColumn(tr("Repeat")); ImGui::TableSetupColumn(tr("Position drift")); ImGui::TableSetupColumn(tr("Rank"));
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < l.size() && i < 12; i++) {
            const auto& e = l[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.hexId.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%s %s", e.beaconType.c_str(), e.country.c_str());
            ImGui::TableNextColumn(); if (e.tests) ImGui::Text("%d (+%d %s)", e.bursts, e.tests, tr("self-test")); else ImGui::Text("%d", e.bursts);
            ImGui::TableNextColumn();
            if (e.medianGapS > 0) ImGui::Text("%.0f s%s", e.medianGapS, e.plausibleRepeat ? "" : (e.bursts >= 4 ? " (!)" : "")); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (e.hasPos) ImGui::Text("%.1f km", e.driftKm); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (e.alert) ImGui::TextColored(pal::warnAmber(), "%s", tr("Look at this")); else ImGui::TextDisabled("%s", e.bursts < 2 && e.tests ? tr("test") : tr("watch"));
        }
        ImGui::EndTable();
    }
}
