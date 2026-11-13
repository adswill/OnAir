// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mesh coverage (3.9, docs/SMART5-iot-3.9.md): direct receptions of nodes with a position, kept as 1 km grid cells (dect2/mesh/mesh_coverage.h).
// No node identity goes into the cells; the packet counter per node below only stops the same packet being counted twice and lives in memory.
#include "app.h"
#include "dect2/mesh/mesh_coverage.h"
#include "dect2/automation/engine.h"
#include <algorithm>
#include <unordered_map>

namespace {
dect2::MeshCoverage gCov(1.0);
std::unordered_map<std::string, uint32_t> gFed;
}

namespace meshui {   // declared in mesh_ui.cpp inside this namespace

void meshCoverageFeed(const dect2::MeshTelemetry& t, double rxLat, double rxLon, bool rxSet) {
    if (rxSet) gCov.setReceiver(rxLat, rxLon);
    const int sf = t.packets.empty() ? 11 : t.packets.front().sf;
    for (const auto& n : t.nodes) {
        if (!n.hasPosition || n.hopsAway != 0) continue;   // heard directly only
        uint32_t& last = gFed[n.id];
        if (n.packets == last) continue;
        last = n.packets;
        gCov.add(n.lat, n.lon, n.lastSnrDb, sf, true, (int64_t)dect2::automation::wallNow());
    }
}

void meshCoverageCard() {
    ImGui::Separator();
    ImGui::TextDisabled("%s", tr("Coverage from direct receptions (1 km cells)"));
    auto cells = gCov.cells();
    if (cells.empty()) { ImGui::TextDisabled("%s", tr("no direct reception with a position yet")); return; }
    dect2::CoverageCell f;
    if (gCov.farthest(f, 1)) ImGui::Text("%zu %s, %s %.1f km (%.0f dB SNR)", cells.size(), tr("cells"), tr("farthest"), f.km, f.bestSnr);
    std::sort(cells.begin(), cells.end(), [](const dect2::CoverageCell& a, const dect2::CoverageCell& b) { return a.km > b.km; });
    if (!ImGui::BeginTable("##meshcov", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, std::max(80.f, ImGui::GetContentRegionAvail().y)))) return;
    ImGui::TableSetupColumn("km"); ImGui::TableSetupColumn(tr("Heard")); ImGui::TableSetupColumn(tr("SNR median / best")); ImGui::TableSetupColumn(tr("Margin"));
    ImGui::TableHeadersRow();
    for (const auto& c : cells) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); if (c.km >= 0) ImGui::Text("%.1f", c.km); else ImGui::Text("%.3f %.3f", c.lat, c.lon);
        ImGui::TableNextColumn(); ImGui::Text("%d", c.count);
        ImGui::TableNextColumn(); ImGui::Text("%.1f / %.1f", c.medianSnr, c.bestSnr);
        ImGui::TableNextColumn(); ImGui::TextColored(c.marginDb < 3 ? ImVec4(0.95f, 0.6f, 0.25f, 1) : ImVec4(0.4f, 0.85f, 0.5f, 1), "%.1f dB", c.marginDb);
    }
    ImGui::EndTable();
}

} // namespace meshui
