// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sweep scanner: activity map per cell and hour (saved, merged over days), new-signal alert against it, classification of the persistent signals. docs/WIRING-4.0.md
#include "d40_ui.h"
#include "../ui/typography.h"
#include "dect2/d40/scan_d40.h"
#include "dect2/sweep/sweep.h"
#include "dect2/sweep/survey.h"
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>

namespace d4 = dect2::d40;

namespace {
std::mutex mu;
d4::ActivityMap map;
d4::NewSignalWatch watch;
std::deque<std::string> alerts;
bool loaded = false, on = true;
int sweepsSession = 0;
double lastSave = 0;

std::string path() { return plat::dataDir() + "/sweep_activity.txt"; }
void load() {
    if (loaded) return;
    loaded = true;
    on = plat::prefs().getB("d40ScanMap", true);
    std::ifstream f(path());
    if (f) { std::stringstream b; b << f.rdbuf(); map.load(b.str()); }
}
void save() { std::ofstream f(path(), std::ios::trunc); if (f) f << map.save(); }
}  // namespace

// called by the sweep thread after every sweep
void d40ScanFeed(const dect2::sweep::Sweep& s) {
    std::lock_guard<std::mutex> l(mu);
    load();
    if (!on || s.bins() == 0) return;
    std::vector<double> hits;
    for (const auto& d : s.signals) hits.push_back(d.freqHz);
    const std::time_t t = (std::time_t)s.timeSec;
    const int hour = std::localtime(&t)->tm_hour;
    const double hi = s.freqOf(s.bins() - 1);
    for (const auto& al : watch.check(map, hour, hits)) {   // before the sweep joins the baseline
        alerts.push_front(al.text);
        if (alerts.size() > 20) alerts.pop_back();
    }
    map.addSweep(hour, s.startHz, hi, hits);
    sweepsSession++;
    if (s.timeSec - lastSave > 60) { lastSave = s.timeSec; save(); }
}

void d40ScanCard(const std::vector<dect2::sweep::PersistentSignal>& persist) {
    std::lock_guard<std::mutex> l(mu);
    load();
    if (!typo::collapsingHeader(trw("Activity map and signal names"), 0)) return;
    if (ImGui::Checkbox(tr("Keep an activity map"), &on)) plat::prefs().setB("d40ScanMap", on);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Counts, per 25 kHz cell and hour of the day, how often a signal was there. Saved between runs and merged over days. A signal in a cell that was almost always quiet at this hour raises an alert."));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Save now"))) save();
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Forget the map"))) { map = d4::ActivityMap(); alerts.clear(); save(); }
    ImGui::TextDisabled("%d %s", sweepsSession, tr("sweeps added this session; alerts need 20 sweeps at the same hour"));
    for (const auto& t : alerts) ImGui::TextColored(pal::warnAmber(), "%s", t.c_str());
    if (!persist.empty() && ImGui::BeginTable("##d40cls", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY, ImVec2(0, 130 * gUi))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("MHz")); ImGui::TableSetupColumn(tr("Looks like")); ImGui::TableSetupColumn(tr("Sure")); ImGui::TableSetupColumn(tr("Busy hours"));
        ImGui::TableHeadersRow();
        for (const auto& p : persist) {
            d4::HitFeatures f; f.centreHz = p.freqHz; f.bwHz = p.bwHz; f.duty = p.occupancy; f.peakToMeanDb = p.maxDb - p.meanDb; f.levelStdDb = (p.maxDb - p.meanDb) / 2;
            const auto c = d4::classifyHit(f);
            const auto bh = map.busyHours(p.freqHz);
            std::string hrs;
            for (size_t i = 0; i < bh.size() && i < 8; i++) { char b[8]; snprintf(b, sizeof b, "%s%d", i ? "," : "", bh[i]); hrs += b; }
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%.4f", p.freqHz / 1e6);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c[0].what.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c[0].why.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.0f %%", c[0].confidence * 100);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(hrs.c_str());
        }
        ImGui::EndTable();
    }
}
