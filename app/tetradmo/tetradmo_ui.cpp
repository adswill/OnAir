// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRA Direct Mode screens: the calls heard, the DSB / DNB counts, the messages. Clear speech goes through the same user-installed TETRA codec as the TETRA mode; encrypted calls are listed and left alone.
#include "app.h"
#include "i18n/i18n.h"
#include "dect2/tetra/tetra_voice.h"
#include "dect2/tetradmo/tetradmo_rx.h"
#include "dect2/tetradmo/tetradmo_tel.h"
#include <cstdio>
#include <string>

dect2::tetra::SpeechFrameSink* tetraSharedVoiceSink(App& a);   // tetra_ui.cpp

namespace {

bool live(const App& a) { return a.rx.standard == 322; }   // family 321, engine standard 323, active standard 322

void tick(App& a) {
    if (!live(a) || !a.engine.running()) return;
    a.engine.tetradmo().setVoiceSink(tetraSharedVoiceSink(a));
}

void tab(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", tr(a.engine.running() ? "starting" : "tune to a TETRA Direct Mode channel and start the receiver")); return; }
    const dect2::TetraDmoTelemetry& t = a.rx.tetradmo;
    ImGui::TextUnformatted(tr("TETRA Direct Mode: terminal to terminal, or through a DM repeater or gateway"));
    ImGui::Spacing();
    ImGui::Text("%s: %s   DSB: %llu   DNB: %llu   %s: %llu", tr("Lock"), t.locked ? tr("yes") : tr("no"), (unsigned long long)t.dsb, (unsigned long long)t.dnb, tr("Bad blocks"), (unsigned long long)t.blocksBad);
    if (t.commType >= 0) ImGui::Text("%s: %s   %s: %d   %s: %d", tr("Link"), dect2::tetraDmoCommName(t.commType), tr("Frame"), t.frame, tr("Slot"), t.slot);
    ImGui::Text("%s: %.0f dBFS", tr("Level"), t.levelDb);
    if (t.encState > 0) ImGui::TextColored(pal::warnAmber(), "%s", tr("Air interface encryption is on: traffic is not decoded"));
    ImGui::Text("%s: %llu  %s: %llu", tr("Voice bursts"), (unsigned long long)t.traffic, tr("Not decoded (encrypted or unknown)"), (unsigned long long)t.trafficRefused);
    ImGui::TextDisabled("%s", tr("Speech: the user-installed TETRA codec, clear calls only. Nothing encrypted is decoded."));
    ImGui::Spacing();
    ImGui::SeparatorText(tr("Calls"));
    if (ImGui::BeginTable("dmocalls", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, 200 * gUi))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("State")); ImGui::TableSetupColumn(tr("Source")); ImGui::TableSetupColumn(tr("Destination")); ImGui::TableSetupColumn("MNI"); ImGui::TableSetupColumn(tr("Seconds")); ImGui::TableSetupColumn(tr("Notes"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const dect2::TetraDmoCall& c : t.calls) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(c.active ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", c.active ? tr("on air") : tr("ended"));
            ImGui::TableNextColumn(); ImGui::Text("%u", c.src);
            ImGui::TableNextColumn(); ImGui::Text("%s%u", c.group ? "G " : "", c.dest);
            ImGui::TableNextColumn(); ImGui::Text("%u/%u", c.mni >> 14, c.mni & 0x3FFF);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", c.durSec);
            ImGui::TableNextColumn(); ImGui::Text("%s%s%s", dect2::tetraDmoCommName(c.commType), c.encrypted ? tr(", encrypted") : "", c.voice ? tr(", voice") : "");
        }
        ImGui::EndTable();
    }
}

void receiver(App& a) { if (live(a)) ImGui::TextUnformatted(dect2::tetraDmoSummary(a.rx.tetradmo).c_str()); }

void list(App& a) {
    if (!live(a)) return;
    const dect2::TetraDmoTelemetry& t = a.rx.tetradmo;
    if (ImGui::BeginTable("dmomsg", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn(tr("Time (s)")); ImGui::TableSetupColumn(tr("Burst")); ImGui::TableSetupColumn(tr("Slot")); ImGui::TableSetupColumn(tr("Message"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const dect2::TetraDmoRow& r : t.rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%.1f", r.timeSec);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.burst.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d/%d", r.frame, r.slot);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.text.c_str());
        }
        ImGui::EndTable();
    }
}

void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State"));
    ImGui::SameLine();
    ImGui::TextUnformatted(live(a) ? dect2::tetraDmoSummary(a.rx.tetradmo).c_str() : tr(a.engine.running() ? "starting" : "stopped"));
}

void summary(const App& a, std::string& l1, std::string& l2) { l1 = "TETRA DMO"; if (live(a)) l2 = dect2::tetraDmoSummary(a.rx.tetradmo); }

} // namespace

extern const ModeUi kTetraDmoUi;
const ModeUi kTetraDmoUi = { .sideTitle = "MESSAGES", .tabName = "TETRA DMO", .tabIcon = Ic::Radio, .tab = tab, .receiver = receiver, .list = list, .status = status, .summary = summary, .tick = tick };
