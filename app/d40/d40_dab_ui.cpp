// DAB: service ranking by sustained quality, transmitter table from the user's TII CSV. docs/WIRING-4.0.md
#include "d40_ui.h"
#include "../smart5/smart5_ui.h"
#include "../ui/typography.h"
#include "dect2/d40/dab_d40.h"
#include <cstdio>

namespace d4 = dect2::d40;

namespace {
d4::ServiceRanker rank;
d4::TiiTable tii;
int tiiRows = 0;
bool tiiLoaded = false;
double lastFeed = 0;
}  // namespace

void d40DabTick(App& a) {
    if (a.rx.standard != 3 || !a.engine.running()) return;
    if (!s5ui::every(lastFeed, 5.0)) return;
    const DabTelemetry& d = a.rx.dab;
    if (!d.ensemble) return;
    const bool locked = d.state == 2;
    const DabEnsemble ens = a.engine.dabEnsemble();
    for (const auto& kv : ens.services) rank.add(kv.first, kv.second.label, locked, d.snrDb);   // the ensemble's SNR: every service of it shares the signal
}

void d40DabCard(App& a) {
    if (!typo::collapsingHeader(trw("Service ranking and transmitters"), 0)) return;
    if (!tiiLoaded) { tiiLoaded = true; tiiRows = tii.load(s5ui::readData("dab_transmitters.csv")); }
    const auto r = rank.ranking();
    if (r.empty()) ImGui::TextDisabled("%s", tr("Ranking needs 3 readings (15 s) of a locked ensemble."));
    else if (ImGui::BeginTable("##d40rank", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY, ImVec2(0, 130 * gUi))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Service")); ImGui::TableSetupColumn(tr("Mean SNR")); ImGui::TableSetupColumn(tr("Spread")); ImGui::TableSetupColumn(tr("Decoded"));
        ImGui::TableHeadersRow();
        for (const auto& s : r) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s.label.empty() ? "?" : s.label.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.1f dB", s.meanSnr);
            ImGui::TableNextColumn(); ImGui::Text("%.1f dB", s.spreadDb);
            ImGui::TableNextColumn(); ImGui::Text("%.0f %%", s.lockShare * 100);
        }
        ImGui::EndTable();
    }
    ImGui::SameLine(); ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Best first: a high mean SNR that stays steady and is decoded in every reading. Readings are of the ensemble the service belongs to, so ranking compares services across the ensembles you visit."));
    // transmitters
    const DabTelemetry& d = a.rx.dab;
    if (tiiRows == 0) { ImGui::TextDisabled("%s", tr("Transmitter names: put a CSV (main,sub,name,lat,lon[,kW]) at dab_transmitters.csv in the data folder.")); return; }
    if (d.tii.empty()) { ImGui::TextDisabled("%s", tr("No transmitter identification heard yet.")); return; }
    std::vector<std::pair<int, int>> heard;
    for (const auto& t : d.tii) heard.push_back({t.mainId, t.subId});
    double lat = 0, lon = 0; receiverLocation(lat, lon);
    const auto h = tii.identify(heard, lat, lon);
    if (!receiverLocationSet()) ImGui::TextDisabled("%s", tr("Set your position (ADS-B antenna position) for distance and bearing."));
    if (ImGui::BeginTable("##d40tii", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("TII"); ImGui::TableSetupColumn(tr("Name")); ImGui::TableSetupColumn(tr("Distance")); ImGui::TableSetupColumn(tr("Bearing"));
        ImGui::TableHeadersRow();
        for (const auto& x : h) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%02d/%02d", x.mainId, x.subId);
            ImGui::TableNextColumn(); if (x.known) ImGui::TextUnformatted(x.name.c_str()); else ImGui::TextDisabled("%s", tr("not in your list"));
            ImGui::TableNextColumn(); if (x.known && receiverLocationSet()) ImGui::Text("%.0f km", x.distKm); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (x.known && receiverLocationSet()) ImGui::Text("%.0f deg", x.bearingDeg); else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
}
