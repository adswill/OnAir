// UAT (978 MHz): aircraft table, FIS-B weather text, message log.
#include "../panels/photo_links_ui.h"
#include "../ui/typography.h"
#include "app.h"
#include "dect2/uat/uat_rx.h"
#include "dect2/uat/uat_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

struct State {
    bool loaded = false;
    double refLat = 39.0, refLon = -77.0;
    uint32_t sel = 0xFFFFFFFFu;
};
State S;

bool live(const App& a) { return a.rx.standard == 84; }

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    if (d.has("uatLat")) S.refLat = d.getD("uatLat", S.refLat);
    if (d.has("uatLon")) S.refLon = d.getD("uatLon", S.refLon);
}

void setRef(App& a, double la, double lo) {
    S.refLat = std::max(-90.0, std::min(90.0, la));
    S.refLon = std::max(-180.0, std::min(180.0, lo));
    plat::prefs().setD("uatLat", S.refLat); plat::prefs().setD("uatLon", S.refLon);
    savePrefs(a);
}

void tick(App& a) {
    loadState();
    if (live(a)) a.engine.uat().setReference(S.refLat, S.refLon);
}

void tuner(App& a, bool&) {
    double la = S.refLat, lo = S.refLon;
    ImGui::SetNextItemWidth(110 * gUi);
    bool ch = ImGui::InputDouble("##uatlat", &la, 0, 0, "%.4f N");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * gUi);
    ch |= ImGui::InputDouble("##uatlon", &lo, 0, 0, "%.4f E");
    if (ch) setRef(a, la, lo);
}

void aircraftTable(const dect2::UatTelemetry& t) {
    if (ImGui::BeginTable("##uata", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("ICAO"); ImGui::TableSetupColumn("Call"); ImGui::TableSetupColumn("Alt ft");
        ImGui::TableSetupColumn("Speed"); ImGui::TableSetupColumn("Hdg"); ImGui::TableSetupColumn("Range");
        ImGui::TableSetupColumn("Src"); ImGui::TableSetupColumn("Age");
        ImGui::TableHeadersRow();
        for (const auto& ac : t.aircraft) {
            ImGui::TableNextRow();
            char id[16]; snprintf(id, sizeof id, "%06X", ac.icao & 0xFFFFFF);
            ImGui::TableNextColumn();
            if (ImGui::Selectable(id, S.sel == ac.icao, ImGuiSelectableFlags_SpanAllColumns)) S.sel = S.sel == ac.icao ? 0xFFFFFFFFu : ac.icao;
            if (ImGui::BeginPopupContextItem()) { photolinks::aircraftMenuItems(ac.icao & 0xFFFFFF, "", ac.callsign); ImGui::EndPopup(); }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ac.callsign.empty() ? "-" : ac.callsign.c_str());
            ImGui::TableNextColumn(); ImGui::Text(ac.hasAlt ? "%d" : "-", ac.altFt);
            ImGui::TableNextColumn(); ImGui::Text(ac.hasSpeed ? "%d kt" : "-", ac.speedKt);
            ImGui::TableNextColumn(); ImGui::Text(ac.hasHeading ? "%.0f" : "-", ac.headingDeg);
            ImGui::TableNextColumn(); ImGui::Text(ac.hasRange ? "%.1f NM" : "-", ac.distNm);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ac.tisb ? "TIS-B" : "ADS-B");
            ImGui::TableNextColumn(); ImGui::Text("%.0f s", ac.ageSec);
        }
        ImGui::EndTable();
    }
}

void tab(App& a) {
    const dect2::UatTelemetry& t = a.rx.uat;
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("Start the receiver.")); }
    else if (t.aircraft.empty() && t.fisb.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", tr("No UAT message yet. UAT is on 978 MHz and is used in the United States; aircraft and FIS-B ground stations appear here as they are heard.")); ImGui::PopTextWrapPos(); }
    if (typo::beginTabItem("Aircraft")) { aircraftTable(t); ImGui::EndTabItem(); }
    if (typo::beginTabItem("FIS-B")) {
        for (const auto& p : t.fisb) {
            ImGui::TextDisabled("%s", p.title.c_str());
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(p.text.empty() ? "(no text)" : p.text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Separator();
        }
        ImGui::EndTabItem();
    }
    if (typo::beginTabItem("Log")) {
        for (auto it = t.framesLog.rbegin(); it != t.framesLog.rend(); ++it) {
            ImGui::TextDisabled("%.1f  %s", it->timeSec, it->kind.c_str());
            ImGui::TextWrapped("%s", it->summary.c_str());
        }
        ImGui::EndTabItem();
    }
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    tab(a);
}

void list(App& a) {
    for (const auto& ac : a.rx.uat.aircraft) {
        char b[48]; snprintf(b, sizeof b, "%06X %s", ac.icao & 0xFFFFFF, ac.callsign.empty() ? "" : ac.callsign.c_str());
        ImGui::Selectable(b, S.sel == ac.icao);
    }
}

void status(App& a) {
    const bool on = live(a);
    lamp("Data", !on ? 0 : a.rx.uat.dataValid ? 1 : 2);
    flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::PushFont(a.mono, 0);
    ImGui::TextUnformatted(on ? dect2::uatSummary(a.rx.uat).c_str() : "stopped");
    ImGui::PopFont();
    flowEnd();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "UAT 978 MHz";
    l2 = live(a) ? dect2::uatSummary(a.rx.uat) : "";
}

void synth(App& a, bool& changed) {
    int n = a.tune.synth.modeOpt[0] > 0 ? a.tune.synth.modeOpt[0] : 4;
    if (ImGui::SliderInt("Aircraft", &n, 1, 12)) { a.tune.synth.modeOpt[0] = n; changed = true; }
}

} // namespace

extern const ModeUi kUatUi;
const ModeUi kUatUi = {
    .sideTitle = "AIRCRAFT",
    .tabName = "UAT",
    .tabIcon = Ic::Compass,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .synth = synth,
    .tick = tick,
};
