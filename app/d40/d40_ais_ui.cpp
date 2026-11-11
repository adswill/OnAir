// AIS: coverage by bearing with blind sectors, and the ship type mix. docs/WIRING-4.0.md
#include "d40_ui.h"
#include "../ui/typography.h"
#include "dect2/ais/ais_coverage.h"
#include <cstdio>
#include <ctime>
#include <set>

namespace {
dect2::AisCoverage cov(12);
double stLat = 1e9, stLon = 1e9;
}  // namespace

void d40AisFeed(const AisTelemetry& t, double lat, double lon) {
    if (lat != stLat || lon != stLon) { stLat = lat; stLon = lon; cov = dect2::AisCoverage(12); cov.setStation(lat, lon); }
    const double now = (double)std::time(nullptr);
    for (const auto& v : t.vessels) if (v.hasPos) cov.add(v.lat, v.lon, now);
}

void d40AisRangeCard(const AisTelemetry& t) {
    if (!typo::collapsingHeader(trw("Coverage by bearing and ship types"), 0)) return;
    if (!receiverLocationSet()) ImGui::TextColored(pal::warnAmber(), "%s", tr("Set your position (ADS-B antenna position) for the coverage."));
    const double now = (double)std::time(nullptr);
    const auto sec = cov.sectors(now, 24 * 3600);
    if (ImGui::BeginTable("##d40aiscov", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(tr("Bearing")); ImGui::TableSetupColumn(tr("Reports")); ImGui::TableSetupColumn(tr("Typical nm")); ImGui::TableSetupColumn(tr("Farthest nm"));
        ImGui::TableHeadersRow();
        for (const auto& s : sec) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%03.0f - %03.0f", s.fromDeg, s.toDeg);
            ImGui::TableNextColumn(); ImGui::Text("%d", s.reports);
            ImGui::TableNextColumn(); if (s.reports) ImGui::Text("%.1f", s.typicalKm * 0.5399568); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (s.reports) ImGui::Text("%.1f", s.maxKm * 0.5399568); else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
    const auto blind = cov.blindSectors(now, 24 * 3600);
    if (!blind.empty()) {
        std::string b;
        for (int i : blind) { char q[32]; snprintf(q, sizeof q, "%s%03.0f-%03.0f", b.empty() ? "" : ", ", 360.0 * i / 12, 360.0 * (i + 1) / 12); b += q; }
        ImGui::TextColored(pal::warnAmber(), "%s: %s", tr("Blind sectors (heard on both sides, nothing in between)"), b.c_str());
    }
    ImGui::TextDisabled("%s", tr("Kept for this session; 24 hours of reports."));
    dect2::AisTypeMix mix;
    for (const auto& v : t.vessels) if (v.shipType > 0) mix.add(v.shipType);
    const auto r = mix.ranked();
    if (!r.empty()) {
        std::string s;
        for (const auto& q : r) { char b[64]; snprintf(b, sizeof b, "%s%s %d", s.empty() ? "" : ", ", dect2::aisShipClassName(q.first), q.second); s += b; }
        ImGui::PushTextWrapPos(0); ImGui::Text("%s: %s", tr("Ships by type"), s.c_str()); ImGui::PopTextWrapPos();
    }
}
