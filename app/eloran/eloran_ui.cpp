// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// eLoran / Loran-C screens: the GRI, the stations with their arrival times relative to the master,
// level and phase code, and the averaged pulse envelope. Loran-C only; the eLoran data channel is not decoded.
#include "app.h"
#include "dect2/eloran/eloran_engine.h"
#include <algorithm>
#include <cstdio>
#include <string>

namespace {

bool live(const App& a) { return a.rx.standard == 127; }   // the engine reports its standard code (pollStd)

const char* stateName(int s) {
    return s == 3 ? "locked" : s == 2 ? "GRI found, waiting for a second match" : s == 1 ? "searching for a GRI" : "no samples";
}

void tab(App& a) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Loran-C at 100 kHz: pulse groups of 8 pulses (9 for the master) 1 ms apart, repeating every GRI, "
                        "with the phase codes A and B. Needs a radio or downconverter that tunes to 100 kHz. The eLoran data channel (9th pulse, Eurofix) is not decoded. Receive only.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "tune to 100 kHz (90 - 110 kHz) and start the receiver"); return; }

    const dect2::ElLoranTelemetry& t = a.rx.eloran;
    auto row = [](const char* k, const char* v) {
        ImGui::TextDisabled("%s", k);
        kvColumn(130 * gUi);
        ImGui::TextUnformatted(v);
    };
    char b[160];
    row("state", stateName(t.state));
    if (t.dataValid) snprintf(b, sizeof b, "%d  (%d us)", t.griUs / 10, t.griUs);
    else snprintf(b, sizeof b, "-");
    row("GRI", b);
    if (!t.chain.empty()) row("chain", t.chain.c_str());
    row("lock", t.locked ? "yes" : "no");
    snprintf(b, sizeof b, "%.1f", t.zScore);
    row("repetition strength", b);
    ImGui::Spacing();

    if (t.dataValid && !t.stations.empty()) {
        if (ImGui::BeginTable("loran_stations", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Station");
            ImGui::TableSetupColumn("ToA offset (us)");
            ImGui::TableSetupColumn("Level (dBFS)");
            ImGui::TableSetupColumn("Phase code");
            ImGui::TableSetupColumn("Margin (dB)");
            ImGui::TableSetupColumn("9th pulse");
            ImGui::TableHeadersRow();
            int sec = 0;
            for (const dect2::loran::Station& s : t.stations) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (s.master) ImGui::TextUnformatted("master");
                else ImGui::Text("secondary %d", ++sec);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", s.toaUs);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", (double)s.levelDb);
                ImGui::TableNextColumn();
                ImGui::Text("%c %s", s.code, s.codeMaster ? "(master code)" : "(secondary code)");
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", (double)s.marginDb);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(s.ninth ? "yes" : "no");
            }
            ImGui::EndTable();
        }
        if (!t.masterFound) ImGui::TextDisabled("no master seen: the offsets are relative to the strongest station");
        ImGui::Spacing();
        if (!t.pulseEnv.empty()) {
            float mx = 0;
            for (float v : t.pulseEnv) mx = std::max(mx, v);
            ImGui::TextDisabled("Averaged pulse (first station), %.0f us before the start to %.0f us after", -t.envStartUs, t.envStartUs + t.envStepUs * (double)t.pulseEnv.size());
            ImGui::PlotLines("##loranpulse", t.pulseEnv.data(), (int)t.pulseEnv.size(), 0, nullptr, -0.1f * mx, 1.1f * mx,
                             ImVec2(ImGui::GetContentRegionAvail().x, 110 * gUi));
        }
    } else {
        ImGui::TextDisabled("listening: no group repetition found yet");
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Input %.0f Hz, centre %.4f MHz", t.inputRate, t.centerHz / 1e6);
}

void receiver(App& a) {
    if (!live(a)) return;
    const dect2::ElLoranTelemetry& t = a.rx.eloran;
    ImGui::Text("%s", dect2::elLoranSummary(t).c_str());
    if (t.dataValid) ImGui::Text("level %.1f dBFS, repetition strength %.1f", (double)t.levelDb, t.zScore);
}

void list(App&) {}
void tick(App&) {}

void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State");
    ImGui::SameLine();
    ImGui::TextUnformatted(live(a) ? dect2::elLoranSummary(a.rx.eloran).c_str() : (a.engine.running() ? "starting" : "stopped"));
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "eLoran / Loran-C";
    if (live(a)) l2 = dect2::elLoranSummary(a.rx.eloran);
}

} // namespace

extern const ModeUi kElLoranUi;
const ModeUi kElLoranUi = {
    .sideTitle = "STATIONS",
    .tabName = "Loran-C",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
