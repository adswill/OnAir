// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VDL Mode 4 screens: message log and aircraft list.
#include "../adsb/adsb_map.h"
#include "app.h"
#include "ui/kit/mode_frame.h"
#include "dect2/vdl4/vdl4_tel.h"
#include <cstdio>
#include <cstring>

namespace {

struct State { bool loaded = false; int view = adsbmap::devView(0); uint64_t sel = 0; adsbmap::View map; };
State S;

bool live(const App& a) { return a.rx.standard == 83; }

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.map.zoom = 7;
    S.map.lat = 51.47;
    S.map.lon = -0.45;
}

void tab(App& a) {
    loadState();
    const dect2::Vdl4Telemetry& t = a.rx.vdl4;
    if (!live(a)) ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("Start the receiver."));
    else if (t.aircraft.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", tr("No VDL Mode 4 burst yet. It is used in only a few places, on VHF airband channels; bursts are short and need a line of sight to the transmitter.")); ImGui::PopTextWrapPos(); }
    if (live(a)) kit::modeNav("vdl4v", S.view, {{tr("Messages"), 0}, {tr("Map"), 1}, {tr("Aircraft"), 2}});   // the view selector is part of the tab (as VDL2 and ACARS)
    if (S.view == 0) {
        if (ImGui::BeginTable("vdl4m", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 220 * gUi))) {
            ImGui::TableSetupColumn("Time");
            ImGui::TableSetupColumn("MHz");
            ImGui::TableSetupColumn("From");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Summary");
            ImGui::TableHeadersRow();
            for (const auto& m : t.messages) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", m.timeSec);
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", m.freqHz / 1e6);
                ImGui::TableNextColumn();
                ImGui::Text("%06X", m.src);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(m.msgName.c_str());
                ImGui::TableNextColumn();
                const bool on = m.serial == S.sel;
                if (ImGui::Selectable(m.summary.c_str(), on)) S.sel = m.serial;
            }
            ImGui::EndTable();
        }
        for (const auto& m : t.messages) {
            if (m.serial == S.sel && !m.detail.empty()) ImGui::TextWrapped("%s", m.detail.c_str());
        }
    } else if (S.view == 1) {
        const ImVec2 sz = ImGui::GetContentRegionAvail();
        adsbmap::draw(S.map, sz);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetItemRectMin();
        adsbmap::fullCorner(p0, sz);
        for (const auto& ac : t.aircraft) {
            if (!ac.hasPos) continue;
            const ImVec2 q = adsbmap::project(ac.lat, ac.lon);
            dl->AddCircleFilled(ImVec2(p0.x + q.x, p0.y + q.y), 5 * gUi, IM_COL32(205, 214, 120, 255));
        }
    } else {
        if (ImGui::BeginTable("vdl4a", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Address");
            ImGui::TableSetupColumn("Msgs");
            ImGui::TableSetupColumn("Position");
            ImGui::TableSetupColumn("Last");
            ImGui::TableHeadersRow();
            for (const auto& ac : t.aircraft) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%06X", ac.addr);
                ImGui::TableNextColumn();
                ImGui::Text("%u", ac.messages);
                ImGui::TableNextColumn();
                if (ac.hasPos) ImGui::Text("%.4f %.4f", ac.lat, ac.lon);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f s", t.nowSec - ac.lastHeardSec);
            }
            ImGui::EndTable();
        }
    }
}

void receiver(App& a) {
    if (!live(a)) return;
    if (resultsClearButton("vdl4")) a.engine.vdl4().clearResults();
}

void list(App&) {}
void status(App& a) { if (live(a)) ImGui::TextUnformatted(dect2::vdl4Summary(a.rx.vdl4).c_str()); }
void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "VDL Mode 4";
    l2 = live(a) ? dect2::vdl4Summary(a.rx.vdl4) : "";
}
void tuner(App&, bool&) {}
void synth(App&, bool&) {}
void tick(App&) {}
void meters(const App& a, std::vector<ModeMeter>& out) {
    const auto& t = a.rx.vdl4;
    out.push_back({"FRAMES", "%.0f", (double)t.blocksOk, 0, 200, t.blocksOk ? 1 : 0});
    out.push_back({"AIRCRAFT", "%.0f", (double)t.aircraft.size(), 0, 30, t.aircraft.empty() ? 0 : 1});
}

} // namespace

extern const ModeUi kVdl4Ui;
const ModeUi kVdl4Ui = {
    .sideTitle = "VDL4",
    .tabName = "Messages",
    .tabIcon = Ic::Doc,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .synth = synth,
    .tick = tick,
    .meters = meters,
};
