// LTR trunking screens: lock, good / bad word counts, the area, the message list and the repeater table.
// Control data only; words are listed only when the 7 check bits verify.
#include "app.h"
#include "dect2/ltr/ltr_tel.h"
#include <cstdio>
#include <string>

namespace {

bool live(const App& a) { return a.rx.standard == 130; }   // family 129, engine standard 131, active standard 130

void tab(App& a) {
    if (!live(a)) {
        ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "tune to an LTR repeater channel (VHF / UHF) and start the receiver");
        return;
    }
    const dect2::LtrTelemetry& t = a.rx.ltr;
    ImGui::TextUnformatted("LTR trunking: 300 bit/s subaudible control words, no voice");
    ImGui::Spacing();
    ImGui::Text("Lock: %s", t.locked ? "yes" : "no");
    ImGui::Text("Good words: %llu   Bad words: %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    if (t.area >= 0) ImGui::Text("Area: %d", t.area); else ImGui::TextDisabled("Area: -");
    ImGui::Text("System: %zu repeaters seen", t.repeaters.size());
    ImGui::Text("Level: %.0f dBFS, data swing %.0f Hz", t.levelDb, t.deviationHz);
    ImGui::Spacing();
    ImGui::TextUnformatted("Repeaters");
    if (ImGui::BeginTable("ltrrep", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Repeater");
        ImGui::TableSetupColumn("Go-to");
        ImGui::TableSetupColumn("Home");
        ImGui::TableSetupColumn("Free");
        ImGui::TableSetupColumn("Last (s)");
        ImGui::TableHeadersRow();
        for (const dect2::LtrRepeater& r : t.repeaters) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d", r.ch);
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)r.asGoto);
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)r.asHome);
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)r.asFree);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", t.timeSec - r.lastSec);
        }
        ImGui::EndTable();
    }
    if (!t.dataValid) ImGui::TextDisabled("listening: no word with a good sync and plausible fields yet");
    wideCarriersTable(a, 0);   // whole span: every repeater of the site in the span
}

void receiver(App& a) {
    if (!live(a)) return;
    const dect2::LtrTelemetry& t = a.rx.ltr;
    ImGui::Text("%s", dect2::ltrSummary(t).c_str());
    ImGui::Text("good %llu, bad %llu, level %.1f dBFS", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.levelDb);
}

void list(App& a) {
    if (!live(a)) return;
    const dect2::LtrTelemetry& t = a.rx.ltr;
    if (ImGui::BeginTable("ltrmsg", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Talkgroup");
        ImGui::TableSetupColumn("Go-to");
        ImGui::TableSetupColumn("Free");
        ImGui::TableSetupColumn("Check");
        ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn("Time (s)");
        ImGui::TableHeadersRow();
        for (const dect2::LtrMessage& m : t.messages) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m.talkgroup.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", m.gotoCh);
            ImGui::TableNextColumn(); ImGui::Text("%d", m.freeCh);
            ImGui::TableNextColumn(); ImGui::Text("%02X", m.chk);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m.kind.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.1f", m.timeSec);
        }
        ImGui::EndTable();
    }
}

void tick(App&) {}

void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State");
    ImGui::SameLine();
    ImGui::TextUnformatted(live(a) ? dect2::ltrSummary(a.rx.ltr).c_str() : (a.engine.running() ? "starting" : "stopped"));
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "LTR trunking";
    if (live(a)) l2 = dect2::ltrSummary(a.rx.ltr);
}

} // namespace

extern const ModeUi kLtrUi;
const ModeUi kLtrUi = {
    .sideTitle = "MESSAGES",
    .tabName = "LTR",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
