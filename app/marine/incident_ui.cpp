// Sea distress incidents: one card per incident with the position (DSC and AIS), distance and bearing from the receiver, the AIS-SART / MOB / EPIRB
// devices joined to it, the vessels that can help, the NAVTEX warnings that mention the scene, and a timeline. Nothing here is sent anywhere.
#include "app.h"
#include "incident_ui.h"
#include "../ui/theme.h"
#include "dect2/marine/marine_incident.h"
#include <cstdio>
#include <ctime>

namespace {

std::string clockText(double t) {
    if (t <= 0) return "-";
    const time_t s = (time_t)t;
    struct tm g{};
#ifdef _WIN32
    gmtime_s(&g, &s);
#else
    gmtime_r(&s, &g);
#endif
    char b[16];
    snprintf(b, sizeof b, "%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

std::string latlon(double lat, double lon) {
    char b[48];
    snprintf(b, sizeof b, "%.4f%c %.4f%c", lat >= 0 ? lat : -lat, lat >= 0 ? 'N' : 'S', lon >= 0 ? lon : -lon, lon >= 0 ? 'E' : 'W');
    return b;
}

} // namespace

void incidentBoardUi(App& a, bool compact) {
    (void)a;
    dect2::marine::IncidentBoard& b = dect2::marine::IncidentBoard::global();
    double la = 0, lo = 0;
    receiverLocation(la, lo);
    b.setOwn(receiverLocationSet(), la, lo);
    const std::vector<dect2::marine::Incident> v = b.snapshot();
    size_t shown = 0;
    for (const auto& i : v) if (!i.test) shown++;
    if (shown == 0) {
        if (!compact) ImGui::TextDisabled("%s", tr("No sea distress incident. A DSC distress alert, an AIS-SART, MOB or EPIRB-AIS opens one; AIS positions, NAVTEX and ships nearby join it by themselves."));
        return;
    }
    for (const auto& i : v) {
        if (i.test) continue;
        ImGui::PushID((int)i.id);
        const ImVec4 col = i.closed ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) : pal::badRed();
        char head[220];
        snprintf(head, sizeof head, "%s %s%s%s", i.closed ? tr("Closed incident") : tr("INCIDENT"), i.kind.c_str(), i.name.empty() ? "" : "  ", i.name.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        const bool open = ImGui::CollapsingHeader((std::string(head) + "  " + i.mmsi + "###inc").c_str(), i.closed ? 0 : ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor();
        if (open) {
            if (i.hasPos) {
                ImGui::TextWrapped("%s %s (%s)", tr("Position"), latlon(i.lat, i.lon).c_str(), i.posSource.c_str());
                if (i.hasOwn) ImGui::Text("%s %.0f nm, %03.0f deg", tr("From you"), i.distNm, i.bearingDeg);
                else ImGui::TextDisabled("%s", tr("Set the receiver position (Settings) to see distance and bearing."));
            } else ImGui::TextColored(pal::badRed(), "%s", tr("No position yet: neither the call nor AIS gave one."));
            if (i.posGapFlag) ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1), "%s %.1f nm (%s %.0f min)", tr("DSC and AIS positions differ by"), i.posGapNm, tr("AIS age"), i.aisAgeSec / 60.0);
            if (!i.nature.empty()) ImGui::Text("%s %s", tr("Nature"), i.nature.c_str());
            ImGui::Text("%s %d%s%s", tr("DSC calls"), i.dscCalls, i.relays ? ("  " + std::string(tr("relays")) + " " + std::to_string(i.relays)).c_str() : "",
                        i.acked ? ("  " + std::string(tr("acknowledged by")) + " " + i.ackedBy).c_str() : "");
            for (const auto& d : i.devices) ImGui::BulletText("%s %s", tr("Device"), d.c_str());
            if (!i.nearby.empty()) {
                ImGui::TextUnformatted(tr("Vessels nearby (they can help)"));
                if (ImGui::BeginTable("##near", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                    ImGui::TableSetupColumn(tr("Vessel")); ImGui::TableSetupColumn("nm"); ImGui::TableSetupColumn(tr("Bearing")); ImGui::TableSetupColumn(tr("Speed")); ImGui::TableSetupColumn(tr("Closest approach"));
                    ImGui::TableHeadersRow();
                    for (const auto& n : i.nearby) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::Text("%s %u", n.name.c_str(), (unsigned)n.mmsi);
                        ImGui::TableNextColumn(); ImGui::Text("%.1f", n.distNm);
                        ImGui::TableNextColumn(); ImGui::Text("%03.0f", n.bearingDeg);
                        ImGui::TableNextColumn(); if (n.sogKn >= 0 && n.sogKn < 102) ImGui::Text("%.0f kn", n.sogKn); else ImGui::TextDisabled("-");
                        ImGui::TableNextColumn();
                        if (n.tcpaMin > 0 && n.sogKn > 0.5) ImGui::Text("%.1f nm in %.0f min", n.dcpaNm, n.tcpaMin); else ImGui::TextDisabled("-");
                    }
                    ImGui::EndTable();
                }
            }
            for (const auto& n : i.navtex) {
                ImGui::TextWrapped("NAVTEX %s: %s", n.header.c_str(), n.reason.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", n.excerpt.c_str());
            }
            if (!compact && ImGui::TreeNode(trw("Timeline"))) {
                for (const auto& e : i.timeline) ImGui::TextWrapped("%s  %-8s %s%s", clockText(e.utc).c_str(), e.source.c_str(), e.text.c_str(), e.count > 1 ? (" x" + std::to_string(e.count)).c_str() : "");
                ImGui::TreePop();
            }
        }
        ImGui::PopID();
    }
}
