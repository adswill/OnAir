// NavIC screen: satellites tracked, time of week and week number from the navigation message. No position fix (core/src/navic, docs/SAT-MISC-3.8.md).
#include "app.h"
#include "dect2/navic/navic.h"
#include <string>

namespace {

bool live(const App& a) { return a.rx.standard == 562; }

void tab(App& a) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("NavIC L5, 1176.45 MHz. Needs an L5 antenna, and the satellites are receivable around India only. Checked on simulated signals only."));
    ImGui::PopTextWrapPos();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver")); return; }
    const dect2::NavicTelemetry& t = a.rx.navic;
    ImGui::Text("%s  (%s: PRN %d)", dect2::navicSummary(t).c_str(), tr("searching"), t.searchPrn);
    if (!ImGui::BeginTable("##nv", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) return;
    ImGui::TableSetupColumn("PRN"); ImGui::TableSetupColumn("C/N0"); ImGui::TableSetupColumn(tr("Doppler")); ImGui::TableSetupColumn(tr("Subframes")); ImGui::TableSetupColumn(tr("Time of week")); ImGui::TableSetupColumn(tr("Week")); ImGui::TableSetupColumn(tr("Alert"));
    ImGui::TableHeadersRow();
    for (const auto& s : t.sats) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%d", s.prn);
        ImGui::TableNextColumn(); ImGui::Text("%.0f", s.cn0);
        ImGui::TableNextColumn(); ImGui::Text("%+.0f", s.dopplerHz);
        ImGui::TableNextColumn(); ImGui::Text("%u", s.frames);
        ImGui::TableNextColumn(); if (s.frames) ImGui::Text("%u s", s.towc * 12); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (s.wn >= 0) ImGui::Text("%d", s.wn); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(s.alert ? tr("yes") : tr("no"));
    }
    ImGui::EndTable();
}
void receiver(App& a) { ImGui::TextUnformatted(live(a) ? dect2::navicSummary(a.rx.navic).c_str() : tr("stopped")); }
void status(App& a) { receiver(a); }
void summary(const App& a, std::string& l1, std::string& l2) { l1 = "NavIC"; if (live(a)) l2 = dect2::navicSummary(a.rx.navic); }

} // namespace

extern const ModeUi kNavicUi;
const ModeUi kNavicUi = {
    .sideTitle = "SATELLITES",
    .tabName = "NavIC",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .status = status,
    .summary = summary,
};
