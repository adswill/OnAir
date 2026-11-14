// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "voice/tgdir_ui.h"
#include "dect2/common/diag.h"
#include "app.h"
#include "i18n/i18n.h"
#include "platform/platform.h"
#include "ui/ui_common.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using dect2::TgKind;

namespace {

std::string filePath() { return plat::dataDir() + "/talkgroups.csv"; }
bool gLoaded = false;
uint64_t gSavedRev = 0;
std::string gMsg;

struct Ed {
    char filter[96] = "";
    char id[16] = "", name[96] = "", group[64] = "", note[160] = "";
    int editId = -1;   // the row being edited (-1: adding)
};
Ed gEd[3];

bool readFile(const std::string& p, std::string& out) {
    std::ifstream f(fs::u8path(p), std::ios::binary);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf(); out = ss.str(); return true;
}

}  // namespace

namespace tgdirui {

static bool gDevOpen = false;
void devOpen() { gDevOpen = true; }

void tick() {
    if (!gLoaded) {
        gLoaded = true;
        std::string t;
        if (readFile(filePath(), t)) dect2::tgEdit([&](dect2::TgDir& d) { d.import(t, TgKind::Dmr); });   // the saved file is in the own format: every row names its kind
        gSavedRev = dect2::tgRevision();
    }
    const uint64_t r = dect2::tgRevision();
    if (r != gSavedRev) {   // an edit: write the whole directory
        gSavedRev = r;
        std::error_code ec; fs::create_directories(fs::u8path(plat::dataDir()), ec);
        std::ofstream f(fs::u8path(filePath()), std::ios::binary | std::ios::trunc);
        if (f) f << dect2::tgSnapshot().exportCsv(true);
        else DIAG(811024, "Writing the talkgroup directory file %s failed (the folder may be read-only), so the learned talkgroup names were not saved.", filePath().c_str());
    }
}

std::string nameOf(TgKind k, uint32_t id) { tick(); return dect2::tgLabel(k, id); }

std::string withName(TgKind k, const std::string& text, uint32_t id) {
    const std::string n = nameOf(k, id);
    return n.empty() ? text : text + "  " + n;
}

void section(TgKind k) {
    tick();
    Ed& e = gEd[(int)k];
    ImGui::PushID(dect2::tgKindName(k));
    if (gDevOpen) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (ImGui::CollapsingHeader(trw("Talkgroup names"))) {
        const dect2::TgDir snap = dect2::tgSnapshot();
        ImGui::TextDisabled(tr("%d names for this system type (%d in all). They are shown beside the talkgroup numbers."), (int)snap.count(k), (int)snap.size());
        ImGui::SetNextItemWidth(70 * gUi);
        ImGui::InputText("##tgid", e.id, sizeof e.id, ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine(); ImGui::SetNextItemWidth(170 * gUi);
        ImGui::InputTextWithHint("##tgname", tr("Name"), e.name, sizeof e.name);
        ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi);
        ImGui::InputTextWithHint("##tggroup", tr("Group"), e.group, sizeof e.group);
        ImGui::SameLine();
        if (ImGui::Button(e.editId >= 0 ? tr("Update##tg") : tr("Add##tg"))) {
            const unsigned long long id = strtoull(e.id, nullptr, 10);
            if (e.id[0] && id <= 0xFFFFFFFFull && e.name[0]) {
                dect2::TgDirEntry x; x.kind = k; x.id = (uint32_t)id; x.name = e.name; x.group = e.group; x.note = e.note;
                dect2::tgEdit([&](dect2::TgDir& d) { d.set(x); });
                e.id[0] = e.name[0] = e.group[0] = e.note[0] = 0; e.editId = -1; gMsg.clear();
            } else gMsg = tr("Give a number and a name.");
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Adds the talkgroup, or replaces the name when the number is already in the list"));
        if (ImGui::Button(tr("Import CSV ...##tg"))) {
            const std::string p = openFileDialog();
            std::string t;
            if (!p.empty()) {
                if (!readFile(p, t)) gMsg = tr("could not read the file");
                else {
                    size_t sk = 0; size_t n = 0;
                    dect2::tgEdit([&](dect2::TgDir& d) { n = d.import(t, k, &sk); });
                    gMsg = std::to_string(n) + " " + tr("names read") + (sk ? ", " + std::to_string(sk) + " " + tr("rows skipped") : std::string());
                }
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Reads a CSV: the RadioReference talkgroup export (Decimal, Alpha Tag, Description, Tag), or this list's own export. Names for the same number are replaced."));
        flowNext(6 * gUi);
        ImGui::BeginDisabled(snap.count(k) == 0);
        if (ImGui::Button(tr("Export CSV ...##tg"))) {
            const std::string p = saveFileDialog(k == TgKind::Tetra ? "tetra_talkgroups.csv" : k == TgKind::P25 ? "p25_talkgroups.csv" : "dmr_talkgroups.csv");
            if (!p.empty()) { std::ofstream f(fs::u8path(p), std::ios::binary); if (f) { f << snap.exportCsv(false, k); gMsg = tr("exported"); } else { DIAG(522337, "Writing the talkgroup export file %s failed (the folder may be read-only), so nothing was exported.", p.c_str()); gMsg = tr("could not write the file"); } }
        }
        ImGui::EndDisabled();
        flowNext(6 * gUi);
        ImGui::SetNextItemWidth(140 * gUi);
        ImGui::InputTextWithHint("##tgfilter", tr("Search"), e.filter, sizeof e.filter);
        if (!gMsg.empty()) ImGui::TextDisabled("%s", gMsg.c_str());
        const auto rows = snap.entries(k, e.filter);
        if (!rows.empty() && ImGui::BeginTable("##tgtab", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, std::min<float>(220 * gUi, (rows.size() + 1) * ImGui::GetTextLineHeightWithSpacing() + 6)))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(tr("Talkgroup")); ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn(tr("Group")); ImGui::TableSetupColumn(""); ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            for (const auto& r : rows) {
                ImGui::PushID((int)r.id);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%u", r.id);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.name.c_str());
                if (!r.note.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.note.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.group.c_str());
                ImGui::TableNextColumn();
                if (ImGui::SmallButton(tr("Edit##tg"))) {
                    snprintf(e.id, sizeof e.id, "%u", r.id); snprintf(e.name, sizeof e.name, "%s", r.name.c_str());
                    snprintf(e.group, sizeof e.group, "%s", r.group.c_str()); snprintf(e.note, sizeof e.note, "%s", r.note.c_str()); e.editId = (int)r.id;
                }
                ImGui::TableNextColumn();
                if (ImGui::SmallButton(tr("Delete##tg"))) { const uint32_t id = r.id; dect2::tgEdit([&](dect2::TgDir& d) { d.erase(k, id); }); }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::PopID();
}

}  // namespace tgdirui
