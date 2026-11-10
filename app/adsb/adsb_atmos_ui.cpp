// ADS-B Atmosphere view (3.9, docs/SMART5-avia-3.9.md): the sounding built from the aircraft's winds and temperatures, and the altimetry
// cross-check (an aircraft whose GNSS-minus-barometric difference disagrees with the others at its level; a whole band moving together).
// The logic is in core (dect2/adsb/adsb_atmos.h); this file feeds it from the telemetry and draws it.
#include "adsb_internal.h"
#include "dect2/adsb/adsb_atmos.h"

using namespace adsbui;

namespace adsbui {

namespace {
dect2::AdsbSounding gSound;
dect2::AdsbAltCheck gAlt;
std::deque<dect2::AltFlag> gFlags;
std::deque<dect2::AltShift> gShifts;
}

void atmosFeed(const AdsbTelemetry& t, double now) {
    std::vector<dect2::AltFlag> fl; std::vector<dect2::AltShift> sh;
    for (const auto& ac : t.aircraft) {
        if (ac.ageSec > 5) continue;   // only fresh reports
        gSound.add(ac, now);
        gAlt.add(ac, now, &fl, &sh);
    }
    for (auto& f : fl) { gFlags.push_back(f); if (gFlags.size() > 50) gFlags.pop_front(); }
    for (auto& s : sh) { gShifts.push_back(s); if (gShifts.size() > 20) gShifts.pop_front(); }
}

void atmosView(App& a) {
    (void)a;
    const double now = ImGui::GetTime();
    const dect2::AtmosProfile p = gSound.profile(now);
    { ImGui::PushTextWrapPos(0);
      ImGui::TextDisabled("%s", tr("A sounding from the aircraft: wind and temperature per 1000 ft layer from the last 30 minutes (Comm-B 4,4 / 5,0 / 6,0, within 80 nm)."));
      ImGui::PopTextWrapPos(); }
    if (p.layers.empty()) { ImGui::TextDisabled("%s", tr("No aircraft has sent wind or temperature yet (needs Comm-B replies: an interrogating radar nearby).")); }
    else {
        ImGui::Text("%d aircraft", p.aircraft);
        if (p.hasTropo) { ImGui::SameLine(0, 18 * gUi); ImGui::Text("%s %d ft (%.0f C)", tr("tropopause"), p.tropoFt, p.tropoTempC); }
        if (p.hasFreezing) { ImGui::SameLine(0, 18 * gUi); ImGui::Text("%s %d ft", tr("freezing level"), p.freezingFt); }
        if (p.hasJet) { ImGui::SameLine(0, 18 * gUi); ImGui::Text("%s %03.0f/%.0f kt at %d ft", tr("jet core"), p.jetFromDeg, p.jetKt, p.jetFt); }
    }
    const float half = ImGui::GetContentRegionAvail().y * 0.6f;
    if (!p.layers.empty() && ImGui::BeginTable("##atmos", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, half))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Layer (ft)")); ImGui::TableSetupColumn(tr("Wind")); ImGui::TableSetupColumn(tr("Temp C"));
        ImGui::TableSetupColumn(tr("ISA dev")); ImGui::TableSetupColumn(tr("Lapse")); ImGui::TableSetupColumn(tr("Samples"));
        ImGui::TableHeadersRow();
        for (size_t i = p.layers.size(); i-- > 0;) {   // highest first, like a sounding chart
            const auto& L = p.layers[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d", L.baseFt);
            ImGui::TableNextColumn(); if (L.nWind) ImGui::Text("%03.0f / %.0f kt", L.windFromDeg, L.windKt); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (L.nTemp) ImGui::Text("%.1f", L.tempC); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (L.nTemp) ImGui::Text("%+.1f", L.isaDevC); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            if (L.nTemp && i > 0 && p.layers[i - 1].nTemp) ImGui::Text("%.1f K/km", -(L.tempC - p.layers[i - 1].tempC) / ((L.baseFt - p.layers[i - 1].baseFt) * 0.0003048));
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%d / %d", L.nWind, L.nTemp);
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("Altimetry cross-check (GNSS minus barometric height against the other aircraft at the same level)"));
    if (gFlags.empty() && gShifts.empty()) ImGui::TextDisabled("%s", tr("nothing found"));
    for (auto it = gShifts.rbegin(); it != gShifts.rend(); ++it) ImGui::TextColored(kWarn, "%s", it->text.c_str());
    for (auto it = gFlags.rbegin(); it != gFlags.rend(); ++it) {
        char id[16]; snprintf(id, sizeof id, "%06X", it->icao & 0xFFFFFF);
        ImGui::Text("%s %s FL%03d: %s", id, it->callsign.c_str(), it->altFt / 100, it->text.c_str());
    }
}

} // namespace adsbui
