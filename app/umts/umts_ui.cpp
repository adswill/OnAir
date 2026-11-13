// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// UMTS (3G) cells: the table of cells found, by primary scrambling code. The cell's operator and identity are in the broadcast channel, which is not decoded.
#include "app.h"
#include "i18n/i18n.h"
#include "dect2/umts/umts_rx.h"
#include <cstdio>
#include <string>

namespace {
bool live(const App& a) { return a.rx.standard == 481; }   // the engine reports its standard code minus one

void tab(App& a) {
    ImGui::TextDisabled("%s", tr("Primary scrambling codes heard in this 5 MHz channel, with their pilot level and the operator and cell identity when the broadcast channel is read."));
    char cl[40]; snprintf(cl, sizeof cl, "%s##umts", tr("Clear"));
    ImGui::SameLine(); if (ImGui::Button(cl)) a.engine.umts().clearResults();
    if (!live(a)) { ImGui::TextDisabled("%s", tr("Start the receiver with this mode selected.")); return; }
    const UmtsTelemetry& t = a.rx.umts;
    if (ImGui::BeginTable("##umtscells", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn(tr("Code")); ImGui::TableSetupColumn(tr("Group")); ImGui::TableSetupColumn("Ec/Io"); ImGui::TableSetupColumn(tr("Level")); ImGui::TableSetupColumn(tr("Carrier offset")); ImGui::TableSetupColumn(tr("Blocks")); ImGui::TableSetupColumn("MCC-MNC"); ImGui::TableSetupColumn(tr("Cell identity"));
        ImGui::TableHeadersRow();
        for (auto& c : t.cells) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("%d", c.psc); ImGui::TableNextColumn(); ImGui::Text("%d", c.group); ImGui::TableNextColumn(); ImGui::Text("%.1f dB", c.ecIoDb); ImGui::TableNextColumn(); ImGui::Text("%.1f dB", c.rscpDbfs); ImGui::TableNextColumn(); ImGui::Text("%+.0f Hz", c.cfoHz); ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)c.blocks); ImGui::TableNextColumn(); if (c.mcc >= 0) ImGui::Text("%03d-%0*d", c.mcc, c.mncDigits, c.mnc); ImGui::TableNextColumn(); if (c.cellIdentity >= 0) ImGui::Text("%lld (RNC %lld, cell %lld)", (long long)c.cellIdentity, (long long)(c.cellIdentity >> 16), (long long)(c.cellIdentity & 0xFFFF));
        }
        ImGui::EndTable();
    }
}
void status(App& a) { if (!live(a)) return; ImGui::Text("%zu %s, %llu %s", a.rx.umts.cells.size(), tr("cells"), (unsigned long long)a.rx.umts.scans, tr("scans")); }
void summary(const App& a, std::string& l1, std::string& l2) { l1 = umtsSummary(a.rx.umts); l2.clear(); (void)a; }
}  // namespace

extern const ModeUi kUmtsUi;
const ModeUi kUmtsUi = { .sideTitle = "CELLS", .tabName = "Cells", .tabIcon = Ic::Radio, .tab = tab, .status = status, .summary = summary };
