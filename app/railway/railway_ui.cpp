#include "app.h"
#include "dect2/railway/railway_rx.h"
#include <algorithm>
namespace {
bool tdcs(const App& a) { return a.rx.standard == 148; }
const dect2::RailwayTelemetry& telemetry(const App& a) { return tdcs(a) ? a.rx.tdcs : a.rx.klw; }
bool live(const App& a) { return a.rx.standard == 147 || a.rx.standard == 148; }
void tab(App& a) {
    const bool dispatch = a.family == 147;
    if (ImGui::SmallButton(tr("Clear"))) (dispatch ? a.engine.tdcs() : a.engine.klw()).clearMessages();
    ImGui::SameLine(); ImGui::TextDisabled("1200 bit/s FFSK, BCH(26,16), CRC-16");
    if (!live(a)) { ImGui::TextDisabled("%s", tr("Start the receiver to listen")); return; }
    const auto& t = telemetry(a);
    ImGui::TextUnformatted(dect2::railwaySummary(t, dispatch ? "TDCS" : "KLW").c_str());
    ImGui::TextDisabled(tr("%llu messages, %llu sync words, %llu failed checks"), (unsigned long long)t.blocksOk, (unsigned long long)t.blocks, (unsigned long long)t.blocksBad);
    static int selected = 0;
    if (ImGui::BeginTable("##railwaylog", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0, ImGui::GetContentRegionAvail().y * 0.6f))) {
        ImGui::TableSetupColumn(tr("Train")); ImGui::TableSetupColumn(tr("Locomotive"));
        ImGui::TableSetupColumn(dispatch ? tr("Speed km/h") : tr("Action"));
        ImGui::TableSetupColumn(dispatch ? tr("Km post") : tr("Tail ID"));
        ImGui::TableSetupColumn(dispatch ? tr("Cars") : tr("Pressure kPa")); ImGui::TableHeadersRow();
        for (size_t i = 0; i < t.messages.size(); i++) {
            const auto& m = t.messages[i]; ImGui::TableNextRow(); ImGui::TableNextColumn();
            const auto label = (m.train.empty() ? "-" : m.train) + "##railway" + std::to_string(i);
            if (ImGui::Selectable(label.c_str(), selected == (int)i, ImGuiSelectableFlags_SpanAllColumns)) selected = (int)i;
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m.locomotive.c_str());
            ImGui::TableNextColumn(); if (dispatch) ImGui::Text("%u", m.speedKmh); else ImGui::TextUnformatted(tr(m.action.c_str()));
            ImGui::TableNextColumn(); if (dispatch) { if (m.kilometerValid) ImGui::Text("%.3f", m.kilometer); else ImGui::TextUnformatted(m.testFrame ? tr("Test frame") : "-"); } else ImGui::TextUnformatted(m.tailId.c_str());
            ImGui::TableNextColumn(); if (dispatch) ImGui::Text("%u", m.carCount); else if (m.pressureKpa >= 0) ImGui::Text("%d", m.pressureKpa); else ImGui::TextUnformatted("-");
        }
        ImGui::EndTable();
    }
    if (t.messages.empty()) return;
    selected = std::clamp(selected, 0, int(t.messages.size()) - 1); const auto& m = t.messages[(size_t)selected];
    ImGui::TextDisabled(tr("Corrected bits: %d"), m.correctedBits);
    if (dispatch) {
        ImGui::Text(tr("Station %u, track %u, actual track %u, driver %u"), m.station, m.track, m.actualTrack, m.driver);
        ImGui::Text(tr("Weight %u t, length %.1f"), m.weightTonnes, m.length);
        ImGui::Text(tr("Signal %d, semaphore %d, locomotive role %d"), m.signalType, m.semaphoreType, m.locomotiveRole);
        if (m.kilometerValid) ImGui::TextUnformatted(tr(m.increasingMileage ? "Increasing mileage" : "Decreasing mileage"));
    } else {
        ImGui::Text(tr("Action code: 0x%02X, reserved: 0x%02X"), m.actionCode, m.commandReserved);
        if (!m.parameter.empty()) ImGui::Text(tr("Parameter: %s"), m.parameter.c_str());
        if (!m.clockSync.empty()) ImGui::Text(tr("Clock: %s"), m.clockSync.c_str());
    }
    std::string raw; const char* h = "0123456789ABCDEF"; for (auto b : m.raw) { raw += h[b >> 4]; raw += h[b & 15]; raw += ' '; }
    ImGui::TextWrapped("%s", raw.c_str());
}
void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = a.family == 147 ? "TDCS" : "KLW"; if (live(a)) l2 = dect2::railwaySummary(telemetry(a), l1.c_str());
}
} // namespace
extern const ModeUi kKlwUi, kTdcsUi;
const ModeUi kKlwUi = {.sideTitle = "MESSAGES", .tabName = "KLW", .tabIcon = Ic::Radio, .tab = tab, .summary = summary};
const ModeUi kTdcsUi = {.sideTitle = "MESSAGES", .tabName = "TDCS", .tabIcon = Ic::Radio, .tab = tab, .summary = summary};
