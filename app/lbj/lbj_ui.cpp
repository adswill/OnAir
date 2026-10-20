// LBJ screens: train number, kilometre marker, locomotive ID and a log of recent warnings.
#include "app.h"
#include "dect2/lbj/lbj_rx.h"
#include "dect2/lbj/lbj_tel.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>

namespace {

struct State { int sel = 0; };
State S;

const char* kHelp = N_(
    "LBJ is the Chinese railway train approach warning on about 821.2375 MHz. Every locomotive sends 1200 bit/s POCSAG numeric pages: a short "
    "message every few seconds with the train number, the speed (km/h) and the kilometre post, and longer messages with the locomotive type and "
    "number and the route.\n\nOnAir decodes the pages itself (receive only). A value that is blank had damaged digits; a row in grey had errors "
    "that were corrected, so check it against the next message of the same train.");

bool live(const App& a) { return a.rx.standard == 82; }

std::string ago(int64_t t) {
    if (t <= 0) return "-";
    const long d = (long)(std::time(nullptr) - t);
    char b[32];
    if (d < 60) snprintf(b, sizeof b, "%ld s", std::max(0L, d));
    else if (d < 3600) snprintf(b, sizeof b, "%ld min", d / 60);
    else snprintf(b, sizeof b, "%ld h", d / 3600);
    return b;
}

void helpButton() {
    if (ImGui::SmallButton("?##lbjhelp")) ImGui::OpenPopup("##lbjhelppop");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("What LBJ carries"));
    if (ImGui::BeginPopup("##lbjhelppop")) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32);
        ImGui::TextUnformatted(tr(kHelp));
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
}

void tab(App& a) {
    helpButton();
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Clear"))) a.engine.lbj().clearMessages();
    if (!live(a)) {
        ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("tune to about 821.2375 MHz and start the receiver"));
        return;
    }
    const dect2::LbjTelemetry& t = a.rx.lbj;
    ImGui::TextDisabled("%s", dect2::lbjSummary(t).c_str());
    ImGui::TextDisabled(tr("FM, 1200 bit/s FSK, %.2f Msps"), t.inputRate / 1e6);
    ImGui::TextDisabled(tr("%llu messages, %llu sync words, %llu blank, %llu failed checks"), (unsigned long long)t.blocksOk, (unsigned long long)t.blocks, (unsigned long long)t.emptyFrames, (unsigned long long)t.blocksBad);
    if (t.messages.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled(tr("listening (level %.0f dBFS, offset %+.0f Hz)"), t.levelDb, t.cfoHz);
        return;
    }
    if (ImGui::BeginTable("##lbjlog", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0, ImGui::GetContentRegionAvail().y * 0.7f))) {
        ImGui::TableSetupColumn(tr("Seen"));
        ImGui::TableSetupColumn(tr("Train"));
        ImGui::TableSetupColumn(tr("Speed km/h"));
        ImGui::TableSetupColumn(tr("Km post"));
        ImGui::TableSetupColumn(tr("Direction"));
        ImGui::TableSetupColumn(tr("Locomotive"));
        ImGui::TableSetupColumn(tr("Route"));
        ImGui::TableSetupColumn(tr("Level"));
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < t.messages.size(); i++) {
            const auto& m = t.messages[i];
            ImGui::TableNextRow();
            if (!m.crcOk) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TableNextColumn();
            char l[96];
            snprintf(l, sizeof l, "%s##lbj%zu", ago(m.wallTime).c_str(), i);
            if (ImGui::Selectable(l, S.sel == (int)i, ImGuiSelectableFlags_SpanAllColumns)) S.sel = (int)i;
            if (!m.crcOk && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Some digits were corrected or lost: check this row against the next message of the train"));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m.train.empty() ? "-" : m.train.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m.speedKmh.empty() ? "-" : m.speedKmh.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m.kmMarker.empty() ? "-" : m.kmMarker.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m.direction == "up" ? tr("Up") : (m.direction == "down" ? tr("Down") : "-"));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m.loco.empty() ? "-" : m.loco.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m.route.empty() ? "-" : m.route.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.0f dB", m.rssiDb);
            if (!m.crcOk) ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }
    S.sel = std::clamp(S.sel, 0, int(t.messages.size()) - 1);
    const auto& m = t.messages[(size_t)S.sel];
    if (m.timeSync) ImGui::Text(tr("Network time: %s"), m.networkTime.empty() ? "-" : m.networkTime.c_str());
    if (m.detailed) {
        ImGui::TextUnformatted(tr(m.paired ? "Basic and detail paired" : "Unpaired detail"));
        if (m.positionValid) ImGui::Text("WGS-84: %.4f, %.4f", m.longitude, m.latitude);
        if (!m.locoCode.empty()) ImGui::Text(tr("Locomotive code: %s, serial: %s"), m.locoCode.c_str(), m.locoSerial.c_str());
        if (!m.cabEnd.empty()) ImGui::Text(tr("Cab end (raw): %s"), m.cabEnd.c_str());
    }
}

void receiver(App& a) {
    if (!live(a)) return;
    const dect2::LbjTelemetry& t = a.rx.lbj;
    ImGui::Text("%s", dect2::lbjSummary(t).c_str());
    ImGui::Text(tr("%llu messages, %llu sync words, %llu blank, %llu failed checks"), (unsigned long long)t.blocksOk, (unsigned long long)t.blocks, (unsigned long long)t.emptyFrames, (unsigned long long)t.blocksBad);
    ImGui::Text("input %.2f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
}

void list(App&) {}
void tick(App&) {}

void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State");
    ImGui::SameLine();
    ImGui::TextUnformatted(live(a) ? dect2::lbjSummary(a.rx.lbj).c_str() : (a.engine.running() ? tr("starting") : tr("stopped")));
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "LBJ";
    if (live(a)) l2 = dect2::lbjSummary(a.rx.lbj);
}

} // namespace

extern const ModeUi kLbjUi;
const ModeUi kLbjUi = {
    .sideTitle = "MESSAGES",
    .tabName = "LBJ",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
