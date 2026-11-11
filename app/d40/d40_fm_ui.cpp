// FM: station auto-naming from the census (one named list, PI on two frequencies = one station) and the DX opening alarm. docs/WIRING-4.0.md
#include "d40_ui.h"
#include "../smart5/smart5_ui.h"
#include "../ui/typography.h"
#include "dect2/d40/fm_d40.h"
#include <cstdio>
#include <map>
#include <sstream>

namespace d4 = dect2::d40;

namespace {
struct Fm {
    std::map<long, std::vector<std::string>> ps;   // PS reads per 100 kHz channel
    std::map<long, int> pi;
    std::map<long, double> snr;
    d4::DxOpening dx;
    bool dxOn = false, init = false, learning = false, alarmPosted = false;
    double lastRead = 0;
    int learned = 0;
    d4::DxOpening::Alarm alarm;
} F;

void loadBase() {
    std::istringstream in(s5ui::readData("fm_dx_baseline.tsv"));
    std::string l;
    while (std::getline(in, l)) {
        int pi = 0; double mhz = 0;
        if (sscanf(l.c_str(), "%x %lf", &pi, &mhz) == 2) { F.dx.learn(pi, mhz); F.learned++; }
    }
}
void saveBase(const std::string& add) {
    std::string t = s5ui::readData("fm_dx_baseline.tsv") + add;
    s5ui::writeData("fm_dx_baseline.tsv", t);
}
}  // namespace

void d40FmTick(App& a) {
    if (!a.fmMode || !a.engine.running() || !a.engine.fmCensusOn()) return;
    if (!F.init) { F.init = true; F.dxOn = plat::prefs().getB("d40FmDx", false); loadBase(); }
    if (!s5ui::every(F.lastRead, 1.0)) return;
    std::string add;
    for (const auto& r : a.engine.fmCensusRows()) {
        if (!r.active) continue;
        const long k = std::lround(r.mhz * 10);
        F.snr[k] = std::max<double>(r.aboveDb, 0);
        if (r.pi) F.pi[k] = r.pi;
        if (!r.ps.empty()) { auto& v = F.ps[k]; if (v.size() < 16) v.push_back(r.ps); else { v.erase(v.begin()); v.push_back(r.ps); } }
        if (r.pi && F.learning && r.aboveDb >= 8) { F.dx.learn(r.pi, r.mhz); char b[48]; snprintf(b, sizeof b, "%04X\t%.1f\n", (unsigned)r.pi, r.mhz); add += b; F.learned++; }
        if (r.pi && F.dxOn && !F.learning) F.dx.observe(r.pi, r.mhz, r.aboveDb);
    }
    if (!add.empty()) saveBase(add);
    if (F.dxOn && !F.learning) {
        F.alarm = F.dx.check();
        s5ui::edge(F.alarmPosted, F.alarm.on, "broadcast", "fm_dx_opening", 1, "fm", F.alarm.text);
    }
}

void d40FmCard(App& a) {
    if (!a.engine.fmCensusOn()) return;
    if (!typo::collapsingHeader(trw("Station names and DX alarm"), 0)) return;
    // naming
    std::vector<d4::FmHit> hits;
    for (const auto& q : F.snr) {
        d4::FmHit h; h.mhz = q.first / 10.0; h.snrDb = q.second;
        auto p = F.pi.find(q.first); if (p != F.pi.end()) h.pi = p->second;
        auto s = F.ps.find(q.first); if (s != F.ps.end()) h.psReads = s->second;
        hits.push_back(h);
    }
    const auto st = d4::nameStations(hits);
    if (st.empty()) ImGui::TextDisabled("%s", tr("No station above 6 dB yet."));
    else if (ImGui::BeginTable("##d40fmn", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY, ImVec2(0, 160 * gUi))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("MHz")); ImGui::TableSetupColumn(tr("Name")); ImGui::TableSetupColumn(tr("Also on")); ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (const auto& s : st) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%.1f", s.mhz);
            ImGui::TableNextColumn(); if (s.name.empty()) ImGui::TextDisabled("%s", s.pi ? "..." : "-"); else ImGui::TextUnformatted(s.name.c_str());
            ImGui::TableNextColumn(); { std::string t; for (double m : s.alsoMhz) { char b[24]; snprintf(b, sizeof b, "%s%.1f", t.empty() ? "" : ", ", m); t += b; } ImGui::TextUnformatted(t.c_str()); }
            ImGui::TableNextColumn(); ImGui::PushID((int)(s.mhz * 10)); if (ImGui::SmallButton(tr("Listen"))) fmTune(a, s.mhz); ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The name is the one most reads agree on (8 clean characters). The same programme code on two frequencies within 300 kHz is one station."));
    // DX alarm
    if (ImGui::Checkbox(tr("DX opening alarm"), &F.dxOn)) plat::prefs().setB("d40FmDx", F.dxOn);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Warns when several foreign stations appear that this site does not normally receive (tropo or sporadic-E). Learn this site's normal band first."));
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Learn normal band"), &F.learning) && !F.learning) a.engine.log("FM DX baseline saved");
    if (F.learning) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("learning: leave the census running for a few minutes, then untick")); }
    else if (F.dxOn && F.learned == 0) ImGui::TextColored(pal::warnAmber(), "%s", tr("No baseline yet: tick Learn normal band."));
    if (F.alarm.on && F.dxOn) ImGui::TextColored(pal::warnAmber(), "%s", F.alarm.text.c_str());
}
