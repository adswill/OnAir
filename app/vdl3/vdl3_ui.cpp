// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VDL Mode 3 screens: the burst log and the counts per burst kind.
#include "app.h"
#include "dect2/vdl3/vdl3_tel.h"
#include <cstdio>

namespace {

struct State { uint64_t sel = 0; };
State S;

bool live(const App& a) { return a.rx.standard == 641; }

void tab(App& a) {
    const dect2::Vdl3Telemetry& t = a.rx.vdl3;
    if (!live(a)) ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("Start the receiver."));
    else if (t.bursts.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("No VDL Mode 3 burst yet. VDL Mode 3 was trialled in the United States and never went into service, so live signals are unlikely; the test signal (Synthetic source) has every burst kind."));
        ImGui::PopTextWrapPos();
    }
    if (ImGui::BeginTable("vdl3b", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 240 * gUi))) {
        ImGui::TableSetupColumn(tr("Time"));
        ImGui::TableSetupColumn(tr("Burst"));
        ImGui::TableSetupColumn(tr("Sync"));
        ImGui::TableSetupColumn(tr("SNR"));
        ImGui::TableSetupColumn(tr("CFO"));
        ImGui::TableSetupColumn(tr("Content"));
        ImGui::TableHeadersRow();
        for (const auto& m : t.bursts) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", m.timeSec);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(m.kindName.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(m.sync.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.0f dB", m.snrDb);
            ImGui::TableNextColumn();
            ImGui::Text("%+.0f Hz", m.cfoHz);
            ImGui::TableNextColumn();
            ImGui::PushID((int)m.serial);
            if (ImGui::Selectable(m.summary.c_str(), m.serial == S.sel)) S.sel = m.serial;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    for (const auto& m : t.bursts) {
        if (m.serial != S.sel) continue;
        ImGui::Text("%s  %s, %d symbols, Golay %s (%d bits corrected), EVM %.0f %%", m.kindName.c_str(), m.sync.c_str(), m.symbols,
                    m.golayOk ? tr("good") : tr("failed"), m.golayFixed, m.evmPct);
        ImGui::Text("%s %s", tr("Information bits:"), m.info.c_str());
        if (!m.dataHex.empty()) {
            ImGui::TextUnformatted(m.rsOk ? tr("Data (Reed-Solomon good):") : tr("User information as received:"));
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(m.dataHex.c_str());
            if (!m.dataText.empty()) ImGui::TextUnformatted(m.dataText.c_str());
            ImGui::PopTextWrapPos();
        }
    }
}

void receiver(App& a) {
    if (!live(a)) return;
    const auto& t = a.rx.vdl3;
    ImGui::Text("%s %llu  %s %llu  %s %llu  %s %llu", tr("M uplink"), (unsigned long long)t.mUplink, tr("3T"), (unsigned long long)t.mUplink3T,
                tr("handoff"), (unsigned long long)t.handoff, tr("poll resp."), (unsigned long long)t.pollResp);
    ImGui::Text("%s %llu  %s %llu  %s %llu  %s %llu", tr("M downlink"), (unsigned long long)t.mDownlink, tr("net entry"), (unsigned long long)t.netEntry,
                tr("data"), (unsigned long long)t.vdData, tr("voice"), (unsigned long long)t.vdVoice);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("System data, headers and handoff bits are shown as raw values: their meaning is in ICAO Doc 9805, which is not public. Voice uses the Mode 3 vocoder, which is not decoded."));
    ImGui::PopTextWrapPos();
    if (resultsClearButton("vdl3")) a.engine.vdl3().clearResults();
}

void list(App&) {}
void status(App& a) { if (live(a)) ImGui::TextUnformatted(dect2::vdl3Summary(a.rx.vdl3).c_str()); }
void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "VDL Mode 3";
    l2 = live(a) ? dect2::vdl3Summary(a.rx.vdl3) : "";
}
void tuner(App&, bool&) {}
void synth(App&, bool&) {}
void tick(App&) {}
void meters(const App& a, std::vector<ModeMeter>& out) {
    const auto& t = a.rx.vdl3;
    out.push_back({"BURSTS", "%.0f", (double)t.blocksOk, 0, 500, t.blocksOk ? 1 : 0});
    out.push_back({"SNR", "%.0f", (double)t.snrDb, 0, 40, t.dataValid ? 1 : 0});
}

} // namespace

extern const ModeUi kVdl3Ui;
const ModeUi kVdl3Ui = {
    .sideTitle = "VDL3",
    .tabName = "Bursts",
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
