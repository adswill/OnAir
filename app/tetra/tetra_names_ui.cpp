// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "tetra_names_ui.h"
#include "app.h"
#include "dect2/common/diag.h"
#include "dect2/tetra/tetra_slots.h"
#include "dect2/tetra/tetra_tel.h"
#include "i18n/i18n.h"
#include "platform/platform.h"
#include "ui/ui_common.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using dect2::TetraIdKind;

namespace {

std::string filePath() { return plat::dataDir() + "/tetra_names.csv"; }
bool gLoaded = false;
uint64_t gSavedRev = 0;
std::string gMsg;

struct Ed {
    char filter[96] = "";
    char id[16] = "", name[96] = "", notes[160] = "", site[64] = "", mcc[8] = "", mnc[8] = "", cc[8] = "";
    bool editing = false;
};
Ed gEd;
int gKind = 0;   // 0 ISSI, 1 GSSI

bool readFile(const std::string& p, std::string& out) {
    std::ifstream f(fs::path(reinterpret_cast<const char8_t*>(p.c_str())), std::ios::binary);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf(); out = ss.str(); return true;
}
int optInt(const char* s) { return s[0] ? atoi(s) : -1; }

void clearForm() { gEd.id[0] = gEd.name[0] = gEd.notes[0] = gEd.site[0] = gEd.mcc[0] = gEd.mnc[0] = gEd.cc[0] = 0; gEd.editing = false; }

// the network of the cell heard last (names with a network apply to it); -1 when none
void currentNet(const App& a, int& mcc, int& mnc) {
    mcc = mnc = -1;
    const dect2::TetraCell* best = nullptr;
    for (const auto& c : a.rx.tetra.cells) if (c.mcc >= 0 && c.mnc >= 0 && (!best || c.lastSec > best->lastSec)) best = &c;
    if (best) { mcc = best->mcc; mnc = best->mnc; }
}

}  // namespace

namespace tetranames {

void tick() {
    if (!gLoaded) {
        gLoaded = true;
        dect2::TetraNames loaded;
        if (dect2::tetraNamesLoadFile(filePath(), loaded)) dect2::tetraNamesEdit([&](dect2::TetraNames& d) { d = loaded; });
        gSavedRev = dect2::tetraNamesRevision();
    }
    const uint64_t r = dect2::tetraNamesRevision();
    if (r != gSavedRev) {
        gSavedRev = r;
        if (!dect2::tetraNamesSaveFile(filePath(), dect2::tetraNamesSnapshot()))
            DIAG(497362, "Writing the TETRA name book %s failed (the folder may be read-only), so the names you entered were not saved.", filePath().c_str());
    }
}

std::string nameOf(TetraIdKind k, uint32_t id, int mcc, int mnc) { tick(); return dect2::tetraNameLabel(k, id, mcc, mnc); }
std::string withName(TetraIdKind k, uint32_t id, int mcc, int mnc) { tick(); return dect2::tetraNameWithId(k, id, mcc, mnc); }

void speakingLine(App& a) {
    tick();
    const dect2::TetraCell* best = nullptr;
    for (const auto& c : a.rx.tetra.cells) if (!best || c.lastSec > best->lastSec) best = &c;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Speaking now"));
    ImGui::SameLine();
    if (!best || !a.engine.running()) { ImGui::TextDisabled("%s", tr("start the receiver")); return; }
    const double now = (double)std::time(nullptr);
    int shown = 0;
    for (int i = 0; i < 4; i++) {
        const dect2::tetra::SlotInfo& k = best->slots[i];
        if (k.state == dect2::tetra::SlotState::Encrypted || k.state == dect2::tetra::SlotState::Unknown || k.state == dect2::tetra::SlotState::Idle) continue;   // an encrypted call carries no ids
        if (k.lastHeardSec < 0 || now - k.lastHeardSec > 8.0 || (!k.issi && !k.talkgroup)) continue;
        if (shown++) { ImGui::SameLine(0, 14 * gUi); }
        std::string s = "";
        if (k.issi) s += "ISSI " + withName(TetraIdKind::Issi, k.issi, best->mcc, best->mnc);
        if (k.issi && k.talkgroup) s += "  ->  ";
        if (k.talkgroup) s += "GSSI " + withName(TetraIdKind::Gssi, k.talkgroup, best->mcc, best->mnc);
        ImGui::Text("%s (%s %d)", s.c_str(), tr("slot"), i + 1);
    }
    if (!shown) ImGui::TextDisabled("%s", tr("nobody heard in the last seconds (encrypted calls show no id)"));
}

void view(App& a) {
    tick();
    const dect2::TetraIdKind k = gKind == 0 ? TetraIdKind::Issi : TetraIdKind::Gssi;
    const dect2::TetraNames snap = dect2::tetraNamesSnapshot();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Your own names for terminal numbers (ISSI) and talkgroup numbers (GSSI), shown beside the numbers on the TETRA screens. The list stays on this computer (tetra_names.csv in the data folder); OnAir ships no names. Import reads a CSV (type, id, name, notes, mcc, mnc, colour_code, site) or plain lines such as 1234;Name, 1234,Name or ISSI=Name."));
    ImGui::PopTextWrapPos();
    ImGui::SetNextItemWidth(90 * gUi);
    if (ImGui::BeginCombo("##tnkind", gKind == 0 ? "ISSI" : "GSSI")) {
        if (ImGui::Selectable("ISSI", gKind == 0)) gKind = 0;
        if (ImGui::Selectable("GSSI", gKind == 1)) gKind = 1;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d %s (%d %s)", (int)snap.count(k), tr("names of this type"), (int)snap.size(), tr("in all"));
    ImGui::SetNextItemWidth(80 * gUi);
    ImGui::InputText("##tnid", gEd.id, sizeof gEd.id, ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine(); ImGui::SetNextItemWidth(170 * gUi);
    ImGui::InputTextWithHint("##tnname", tr("Name"), gEd.name, sizeof gEd.name);
    ImGui::SameLine(); ImGui::SetNextItemWidth(170 * gUi);
    ImGui::InputTextWithHint("##tnnotes", tr("Notes"), gEd.notes, sizeof gEd.notes);
    ImGui::SetNextItemWidth(50 * gUi);
    ImGui::InputTextWithHint("##tnmcc", "MCC", gEd.mcc, sizeof gEd.mcc, ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine(); ImGui::SetNextItemWidth(50 * gUi);
    ImGui::InputTextWithHint("##tnmnc", "MNC", gEd.mnc, sizeof gEd.mnc, ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine(); ImGui::SetNextItemWidth(50 * gUi);
    ImGui::InputTextWithHint("##tncc", tr("CC"), gEd.cc, sizeof gEd.cc, ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine(); ImGui::SetNextItemWidth(130 * gUi);
    ImGui::InputTextWithHint("##tnsite", tr("Site"), gEd.site, sizeof gEd.site);
    ImGui::SameLine();
    if (ImGui::Button(gEd.editing ? tr("Update##tn") : tr("Add##tn"))) {
        const unsigned long long id = strtoull(gEd.id, nullptr, 10);
        const int mcc = optInt(gEd.mcc), mnc = optInt(gEd.mnc);
        if (!gEd.id[0] || id == 0 || id > 0xFFFFFFull || !gEd.name[0]) gMsg = tr("Give a number (1 to 16777215) and a name.");
        else if ((mcc < 0) != (mnc < 0) || mcc > 999 || mnc > 9999) gMsg = tr("A network needs both MCC and MNC (or neither).");
        else {
            dect2::TetraNameEntry x; x.kind = k; x.id = (uint32_t)id; x.name = gEd.name; x.notes = gEd.notes; x.site = gEd.site; x.mcc = mcc; x.mnc = mnc; x.colourCode = optInt(gEd.cc);
            dect2::tetraNamesEdit([&](dect2::TetraNames& d) { d.set(x); });
            clearForm(); gMsg.clear();
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Adds the name, or replaces the name for the same number and network. Leave MCC and MNC empty for a name that applies on every network."));
    ImGui::SameLine();
    if (gEd.editing && ImGui::Button(tr("Cancel##tn"))) clearForm();
    int cmcc, cmnc; currentNet(a, cmcc, cmnc);
    if (cmcc >= 0) { ImGui::TextDisabled("%s MCC %d MNC %d", tr("Network heard now:"), cmcc, cmnc); }
    if (ImGui::Button(tr("Import ...##tn"))) {
        const std::string p = openFileDialog();
        if (!p.empty()) {
            std::string t;
            if (!readFile(p, t)) gMsg = tr("could not read the file");
            else {
                size_t sk = 0, n = 0;
                dect2::tetraNamesEdit([&](dect2::TetraNames& d) { n = d.import(t, k, &sk); });
                gMsg = std::to_string(n) + " " + tr("names read") + (sk ? ", " + std::to_string(sk) + " " + tr("rows skipped") : std::string());
            }
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Reads a CSV with a header, or plain lines (id;name, id,name, ISSI=name). Rows without a type are taken as the type chosen on the left. Names for the same number and network are replaced."));
    flowNext(6 * gUi);
    ImGui::BeginDisabled(snap.size() == 0);
    if (ImGui::Button(tr("Export ...##tn"))) {
        const std::string p = saveFileDialog("tetra_names.csv");
        if (!p.empty()) {
            std::ofstream f(fs::path(reinterpret_cast<const char8_t*>(p.c_str())), std::ios::binary);
            if (f) { f << snap.exportCsv(true); gMsg = tr("exported"); }
            else { DIAG(829354, "Writing the TETRA name export file %s failed (the folder may be read-only), so nothing was exported.", p.c_str()); gMsg = tr("could not write the file"); }
        }
    }
    ImGui::EndDisabled();
    flowNext(6 * gUi);
    ImGui::SetNextItemWidth(140 * gUi);
    ImGui::InputTextWithHint("##tnfilter", tr("Search"), gEd.filter, sizeof gEd.filter);
    if (!gMsg.empty()) ImGui::TextDisabled("%s", gMsg.c_str());
    const auto rows = snap.entries(k, gEd.filter);
    if (rows.empty() || !ImGui::BeginTable("##tntab", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tetraIdKindName(k)); ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn(tr("Notes"));
    ImGui::TableSetupColumn(tr("Network")); ImGui::TableSetupColumn(tr("CC")); ImGui::TableSetupColumn(tr("Site")); ImGui::TableSetupColumn(""); ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();
    for (const auto& r : rows) {
        ImGui::PushID((int)(r.id * 31u) + r.mcc * 7 + r.mnc);
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%u", r.id);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(r.name.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(r.notes.c_str());
        ImGui::TableNextColumn(); if (r.mcc >= 0) ImGui::Text("%d/%d", r.mcc, r.mnc); else ImGui::TextDisabled("%s", tr("any"));
        ImGui::TableNextColumn(); if (r.colourCode >= 0) ImGui::Text("%d", r.colourCode); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(r.site.c_str());
        ImGui::TableNextColumn();
        if (ImGui::SmallButton(tr("Edit##tn"))) {
            snprintf(gEd.id, sizeof gEd.id, "%u", r.id); snprintf(gEd.name, sizeof gEd.name, "%s", r.name.c_str());
            snprintf(gEd.notes, sizeof gEd.notes, "%s", r.notes.c_str()); snprintf(gEd.site, sizeof gEd.site, "%s", r.site.c_str());
            gEd.mcc[0] = gEd.mnc[0] = gEd.cc[0] = 0;
            if (r.mcc >= 0) { snprintf(gEd.mcc, sizeof gEd.mcc, "%d", r.mcc); snprintf(gEd.mnc, sizeof gEd.mnc, "%d", r.mnc); }
            if (r.colourCode >= 0) snprintf(gEd.cc, sizeof gEd.cc, "%d", r.colourCode);
            gEd.editing = true;
        }
        ImGui::TableNextColumn();
        if (ImGui::SmallButton(tr("Delete##tn"))) { const auto rr = r; dect2::tetraNamesEdit([&](dect2::TetraNames& d) { d.erase(rr.kind, rr.id, rr.mcc, rr.mnc); }); }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

}  // namespace tetranames
