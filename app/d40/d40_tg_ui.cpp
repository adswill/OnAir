// Talkgroup activity (airtime ranking, 50-minute timeline, busiest talkgroup) for P25, NXDN and TETRA, as DMR already has. docs/WIRING-4.0.md
#include "d40_ui.h"
#include "../ui/typography.h"
#include "../smart5/smart5_ui.h"
#include "dect2/d40/radio_e40.h"
#include "dect2/trunkpool/tg_activity.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace {
// talkgroup names from talkgroups.csv in the data folder (RadioReference style "dec,hex,mode,alpha,..." or "dec,name"); nothing is bundled
dect2::d40::TgAlias gAlias;
bool gAliasLoaded = false;
int gAliasRows = 0;
void loadAlias(bool force = false) {
    if (gAliasLoaded && !force) return;
    gAliasLoaded = true; gAlias = dect2::d40::TgAlias();
    gAliasRows = gAlias.importCsv(s5ui::readData("talkgroups.csv"));
}
std::string tgText(uint32_t tg) { return gAliasRows ? gAlias.name((int)tg) : "TG " + std::to_string(tg); }

struct TgCall { uint32_t dst, src; double s, e; };

struct Panel {
    dect2::TgActivity act; dect2::TgFollower fol;
    double built = -1, nowS = 0;
};

void draw(Panel& P, const char* id, const std::vector<TgCall>& calls, bool showRadios) {
    const double wall = ImGui::GetTime();
    if (P.built < 0 || wall - P.built > 2.0) {
        P.built = wall; P.act = dect2::TgActivity(); P.nowS = 0;
        for (const auto& c : calls) { P.nowS = std::max(P.nowS, c.e); P.act.add(c.dst, c.src, c.s, c.e); }
    }
    ImGui::PushID(id);
    const auto rows = P.act.ranking(P.nowS, 600);
    if (rows.empty()) { ImGui::TextDisabled("%s", tr("No group calls yet")); ImGui::PopID(); return; }
    loadAlias();
    ImGui::Text("%s: %s", tr("Busiest now"), tgText(P.fol.pick(P.act, P.nowS)).c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The talkgroup with the most airtime in the last 2 minutes. It only changes when another one is clearly busier for a while."));
    if (ImGui::BeginTable("##tgact", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn(tr("Talkgroup")); ImGui::TableSetupColumn(tr("Calls")); ImGui::TableSetupColumn(tr("Airtime s")); ImGui::TableSetupColumn(tr("Radios")); ImGui::TableSetupColumn(tr("Last 50 min"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < rows.size() && i < 8; i++) {
            const auto& r = rows[i];
            ImGui::TableNextRow(); ImGui::TableNextColumn(); if (gAliasRows) ImGui::Text("%u %s", r.tg, tgText(r.tg).c_str()); else ImGui::Text("%u", r.tg);
            ImGui::TableNextColumn(); ImGui::Text("%d", r.calls);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", r.airtimeS);
            ImGui::TableNextColumn(); if (showRadios) ImGui::Text("%d", r.radios); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            const auto tl = P.act.timeline(r.tg, P.nowS, 300, 10);
            std::vector<float> f(tl.begin(), tl.end());
            ImGui::PushID((int)i);
            ImGui::PlotHistogram("##tl", f.data(), (int)f.size(), 0, nullptr, 0.f, 300.f, ImVec2(ImGui::GetContentRegionAvail().x, 18 * gUi));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (gAliasRows) ImGui::TextDisabled("%d %s", gAliasRows, tr("talkgroup names loaded from talkgroups.csv"));
    else ImGui::TextDisabled("%s", tr("Names: put talkgroups.csv (decimal id, name; or a RadioReference export) in the data folder."));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Reload names"))) loadAlias(true);
    ImGui::PopID();
}
}  // namespace

void d40P25Activity(const P25Telemetry& t) {
    static Panel P; static size_t last = 0;
    if (t.log.size() < last && t.log.empty()) P = Panel();
    last = t.log.size();
    std::vector<TgCall> v;
    for (const auto& c : t.log) if (c.kind == 0 && c.group && c.dst >= 0) v.push_back({(uint32_t)c.dst, c.src < 0 ? 0u : (uint32_t)c.src, c.startSec, c.startSec + std::max(0.0, c.durSec)});
    draw(P, "d40p25", v, true);
}

void d40NxdnActivity(const NxdnTelemetry& t) {
    static Panel P; static size_t last = 0;
    if (t.log.size() < last && t.log.empty()) P = Panel();
    last = t.log.size();
    std::vector<TgCall> v;
    for (const auto& c : t.log) if (c.kind == 0 && (c.callType == 1 || c.callType == 0) && c.dst >= 0) v.push_back({(uint32_t)c.dst, c.src < 0 ? 0u : (uint32_t)c.src, c.startSec, c.startSec + std::max(0.0, c.durSec)});
    draw(P, "d40nxdn", v, true);
}

// TETRA has no call log: calls are built from the four timeslots of each cell (clear voice with a known talkgroup). Terminal numbers (ISSI) are never used.
void d40TetraTick(const TetraTelemetry& t);
namespace {
std::vector<TgCall> gTetraCalls;
std::map<long, size_t> gOpen;   // key (cell, slot, tg) -> index into gTetraCalls
Panel gTetraPanel;
}
void d40TetraTick(const TetraTelemetry& t) {
    for (size_t ci = 0; ci < t.cells.size(); ci++)
        for (int s = 0; s < 4; s++) {
            const auto& sl = t.cells[ci].slots[s];
            if (sl.state != dect2::tetra::SlotState::Voice || sl.talkgroup == 0 || sl.lastHeardSec < 0) continue;
            const long key = (long)ci * 100000000L + (long)s * 10000000L + (long)(sl.talkgroup % 10000000);
            auto it = gOpen.find(key);
            if (it != gOpen.end() && sl.lastHeardSec - gTetraCalls[it->second].e <= 3.0) { gTetraCalls[it->second].e = std::max(gTetraCalls[it->second].e, sl.lastHeardSec); continue; }
            if (it != gOpen.end() && sl.lastHeardSec <= gTetraCalls[it->second].e) continue;
            gTetraCalls.push_back({sl.talkgroup, 0, sl.lastHeardSec, sl.lastHeardSec});
            gOpen[key] = gTetraCalls.size() - 1;
            if (gTetraCalls.size() > 2000) { gTetraCalls.erase(gTetraCalls.begin(), gTetraCalls.begin() + 500); gOpen.clear(); }
        }
}
void d40TetraActivity() { draw(gTetraPanel, "d40tetra", gTetraCalls, false); }
