// ADS-B: track replay. Every position fix is kept (bounded) and a slider shows where every aircraft was up to the last hour back. docs/WIRING-4.0.md
#include "d40_ui.h"
#include "../ui/typography.h"
#include "dect2/adsb/adsb_replay.h"
#include <cmath>
#include <cstdio>
#include <ctime>

namespace {
dect2::TrackReplay R;
double minutesBack = 0;
double lastPrune = 0;
}  // namespace

void d40AdsbFeed(const AdsbTelemetry& t) {
    const double now = (double)std::time(nullptr);
    for (const auto& ac : t.aircraft)
        if (ac.hasPos && !ac.ground && ac.posAgeSec <= 3) R.add(ac.icao, now - ac.posAgeSec, ac.lat, ac.lon, ac.hasAlt ? ac.altFt : 0);
    if (now - lastPrune > 60) { lastPrune = now; R.prune(now - 3600); }
}

void d40AdsbReplayView(App& a, double refLat, double refLon, const std::function<std::string(uint32_t)>& nameOf) {
    const double now = (double)std::time(nullptr);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Replay: where every aircraft was a few minutes ago, from the positions received since this window started (up to one hour back, gaps of more than 2 minutes are not bridged)."));
    ImGui::PopTextWrapPos();
    float m = (float)minutesBack;
    ImGui::SetNextItemWidth(-120 * gUi);
    if (ImGui::SliderFloat("##d40rp", &m, 0, 60, tr("%.1f minutes ago"))) minutesBack = m;
    ImGui::SameLine(); if (ImGui::SmallButton(tr("Now"))) minutesBack = 0;
    const auto sky = R.sky(now - minutesBack * 60.0);
    ImGui::Text("%zu %s, %zu %s", sky.size(), tr("aircraft at that time"), R.aircraft(), tr("tracked"));
    if (sky.empty()) { ImGui::TextDisabled("%s", tr("Nothing yet: keep the receiver running.")); return; }
    if (ImGui::BeginTable("##d40replay", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Aircraft")); ImGui::TableSetupColumn(tr("Latitude")); ImGui::TableSetupColumn(tr("Longitude")); ImGui::TableSetupColumn(tr("Altitude ft")); ImGui::TableSetupColumn(tr("Distance km"));
        ImGui::TableHeadersRow();
        for (const auto& s : sky) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string n = nameOf ? nameOf(s.first) : std::string();
            if (n.empty()) ImGui::Text("%06X", (unsigned)(s.first & 0xFFFFFF)); else ImGui::TextUnformatted(n.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.4f%s", s.second.lat, s.second.interpolated ? " ~" : "");
            ImGui::TableNextColumn(); ImGui::Text("%.4f", s.second.lon);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", s.second.altFt);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", dect2::TrackReplay::distKm(refLat, refLon, s.second.lat, s.second.lon));
        }
        ImGui::EndTable();
    }
    (void)a;
}
