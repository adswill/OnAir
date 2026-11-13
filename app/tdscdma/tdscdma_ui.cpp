// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TD-SCDMA cells: the table of cells found, by SYNC-DL code (code group) and cell parameter.
#include "app.h"
#include "i18n/i18n.h"
#include "dect2/tdscdma/tdscdma_rx.h"
#include <cstdio>
#include <string>

namespace {
bool live(const App& a) { return a.rx.standard == 662; }   // the engine reports its standard code minus one

void tab(App& a) {
    ImGui::TextDisabled("%s", tr("TD-SCDMA cells heard in this 1.6 MHz channel, by their downlink pilot (SYNC-DL code) and the cell parameter of their time slot 0 midamble. The broadcast channel is not decoded."));
    char cl[40]; snprintf(cl, sizeof cl, "%s##tds", tr("Clear"));
    ImGui::SameLine(); if (ImGui::Button(cl)) a.engine.tdscdma().clearResults();
    if (!live(a)) { ImGui::TextDisabled("%s", tr("Start the receiver with this mode selected.")); return; }
    const TdscdmaTelemetry& t = a.rx.tdscdma;
    if (ImGui::BeginTable("##tdscells", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("SYNC-DL"); ImGui::TableSetupColumn(tr("Cell parameter")); ImGui::TableSetupColumn(tr("Level")); ImGui::TableSetupColumn("SNR");
        ImGui::TableSetupColumn(tr("Carrier offset")); ImGui::TableSetupColumn(tr("P-CCPCH quadruples")); ImGui::TableSetupColumn(tr("Blocks"));
        ImGui::TableHeadersRow();
        for (auto& c : t.cells) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d", c.syncDl);
            ImGui::TableNextColumn();
            if (c.cellParam >= 0 && c.paramSure) ImGui::Text("%d", c.cellParam);
            else if (c.cellParam >= 0) ImGui::TextDisabled("%d / %d", c.cellParam, c.cellParam ^ 1);
            ImGui::TableNextColumn(); ImGui::Text("%.1f dBFS", c.levelDbfs);
            ImGui::TableNextColumn(); ImGui::Text("%.1f dB", c.snrDb);
            ImGui::TableNextColumn(); ImGui::Text("%+.0f Hz", c.cfoHz);
            ImGui::TableNextColumn(); ImGui::Text("S1 %llu, S2 %llu", (unsigned long long)c.quadS1, (unsigned long long)c.quadS2);
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)c.blocks);
        }
        ImGui::EndTable();
    }
}
void status(App& a) { if (!live(a)) return; ImGui::Text("%zu %s, %llu %s", a.rx.tdscdma.cells.size(), tr("cells"), (unsigned long long)a.rx.tdscdma.scans, tr("scans")); }
void summary(const App& a, std::string& l1, std::string& l2) { l1 = tdscdmaSummary(a.rx.tdscdma); l2.clear(); }
}  // namespace

extern const ModeUi kTdscdmaUi;
const ModeUi kTdscdmaUi = { .sideTitle = "CELLS", .tabName = "Cells", .tabIcon = Ic::Radio, .tab = tab, .status = status, .summary = summary };
