// ACARS: channel load ranking and the fewest channels that carry 90 % of the traffic. docs/WIRING-4.0.md
#include "d40_ui.h"
#include "../ui/typography.h"
#include "dect2/acars/acars_load.h"
#include <cstdio>
#include <ctime>

namespace {
dect2::AcarsLoad load;
uint64_t lastSerial = 0;
}  // namespace

void d40AcarsFeed(const dect2::AcarsTelemetry& t) {
    for (auto it = t.messages.rbegin(); it != t.messages.rend(); ++it) {   // newest first in the telemetry
        if (it->serial <= lastSerial) continue;
        lastSerial = it->serial;
        load.add(it->freqHz / 1e6, it->crcOk, it->wallTime > 0 ? (double)it->wallTime : (double)std::time(nullptr));
    }
}

void d40AcarsLoadCard() {
    if (!typo::collapsingHeader(trw("Channel load"), 0)) return;
    const double now = (double)std::time(nullptr);
    const auto r = load.ranking(now, 1800);
    if (r.empty()) { ImGui::TextDisabled("%s", tr("No messages in the last 30 minutes.")); return; }
    if (ImGui::BeginTable("##d40acl", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(tr("MHz")); ImGui::TableSetupColumn(tr("Per minute")); ImGui::TableSetupColumn(tr("Good")); ImGui::TableSetupColumn(tr("Cumulative share"));
        ImGui::TableHeadersRow();
        for (const auto& c : r) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%.3f", c.freqMHz);
            ImGui::TableNextColumn(); ImGui::Text("%.2f", c.perMin);
            ImGui::TableNextColumn(); ImGui::Text("%.0f %%", c.goodPct);
            ImGui::TableNextColumn(); ImGui::Text("%.0f %%", c.cumulativeSharePct);
        }
        ImGui::EndTable();
    }
    std::string best;
    for (double f : load.bestChannels(now, 1800)) { char b[24]; snprintf(b, sizeof b, "%s%.3f", best.empty() ? "" : ", ", f); best += b; }
    ImGui::TextDisabled("%s: %s MHz", tr("Fewest channels for 90 % of the traffic"), best.c_str());
}
