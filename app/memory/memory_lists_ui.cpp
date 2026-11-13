// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sharing and organising memories: the import preview (formats other programs use, duplicates, merge rule), "Export as...", the "My lists" manager
// (enable / order / rename lists, add a built-in list of common services) and the quick-tune bar of starred memories. The models are in core
// (dect2/channels/memory_formats.h); memory_ui.cpp draws the panel these pieces sit in.
#include "memory_ui.h"
#include "../ui/ui_common.h"
#include "dect2/channels/memory_formats.h"
#include "dect2/common/u8file.h"
#include <algorithm>
#include <fstream>
#include <sstream>

namespace {
using dect2::ImportPreview;
using dect2::ListFormat;
using dect2::ListSet;
using dect2::MergeRule;
using dect2::Memory;
using dect2::MemoryBank;

struct S {
    ListSet lists;
    bool loaded = false;
    // the import preview
    bool open = false;
    ImportPreview prev;
    std::string prevFile;
    int rule = 1;                       // MergeRule order below; default: keep what the bank has
    int exportFmt = 0;
    int builtinSel = 0, regionSel = 0;
    char renameTo[64] = "";
    std::string renaming;
    std::string msg;
    bool msgBad = false;
    double msgT = 0;
} s;

constexpr ListFormat kExportFormats[] = {ListFormat::OnAirJson, ListFormat::OnAirCsv, ListFormat::SdrppJson, ListFormat::SdrSharpXml, ListFormat::SdrCsv, ListFormat::ChirpCsv};
constexpr MergeRule kRules[] = {MergeRule::KeepBoth, MergeRule::KeepExisting, MergeRule::Replace, MergeRule::FillEmpty};

std::string listsPath() { return plat::dataDir() + "/memory-lists.json"; }

std::string slurp(const std::string& p, bool& ok) {
    std::ifstream f(dect2::u8file(p), std::ios::binary);
    ok = (bool)f;
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
bool spit(const std::string& p, const std::string& t) {
    std::ofstream f(dect2::u8file(p), std::ios::binary);
    f << t;
    f.flush();
    return (bool)f;
}

void say(const std::string& m, bool bad = false) { s.msg = m; s.msgBad = bad; s.msgT = ImGui::GetTime(); }

void saveLists() { spit(listsPath(), s.lists.toJson()); }

void ensure() {
    if (s.loaded) return;
    s.loaded = true;
    bool ok = false;
    const std::string t = slurp(listsPath(), ok);
    if (ok) s.lists.fromJson(t);
}

// a preview from text; opens the popup
void beginPreview(const MemoryBank& b, const std::string& text, const std::string& name, const std::string& group) {
    s.prev = dect2::previewImport(b, text, ListFormat::Unknown, group);
    s.prevFile = name;
    if (!s.prev.rep.ok) { say(std::string(tr("Import failed: ")) + s.prev.rep.error, true); return; }
    s.open = true;
}

const char* ruleName(int i) {
    static const char* n[] = {N_("Add all (keep both)"), N_("Skip the ones I have"), N_("Replace mine with the file's"), N_("Fill in what mine is missing")};
    return tr(n[i]);
}

}  // namespace

namespace memui {

dect2::ListSet& memoryLists() { ensure(); return s.lists; }

bool memoryListEnabled(const std::string& group) { ensure(); return s.lists.enabled(group); }

// the import button: asks for a file and opens the preview
void importButton(MemoryBank& b) {
    if (ImGui::Button(tr("Import..."))) {
        const std::string p = openFileDialog();
        if (!p.empty()) {
            bool ok = false;
            const std::string t = slurp(p, ok);
            if (!ok) say(std::string(tr("Could not read ")) + p, true);
            else beginPreview(b, t, p, "");
        }
    }
    if (ImGui::IsItemHovered()) wrapTooltip(380 * gUi, tr("Reads OnAir memories (JSON or CSV), SDR++ Frequency Manager, SDR# frequency lists, CHIRP CSV, plain frequency CSV files or 'frequency,name' lines. You see what is new and what you have already before anything is added."));
}

// "Export as": a format and a button
void exportAs(MemoryBank& b) {
    ImGui::SetNextItemWidth(190 * gUi);
    if (ImGui::BeginCombo("##memexpfmt", dect2::listFormatName(kExportFormats[s.exportFmt]))) {
        for (int i = 0; i < 6; i++) if (ImGui::Selectable(dect2::listFormatName(kExportFormats[i]), s.exportFmt == i)) s.exportFmt = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Export as..."))) {
        const ListFormat f = kExportFormats[s.exportFmt];
        const std::string p = saveFileDialog((std::string("onair-memories") + dect2::listFormatExt(f)).c_str());
        if (!p.empty()) {
            std::vector<Memory> v;
            for (size_t i : s.lists.visible(b)) v.push_back(b.all()[i]);   // what the lists show, in the lists' order
            say(spit(p, dect2::writeList(v, f)) ? std::string(tr("Exported to ")) + p : std::string(tr("Could not write ")) + p, false);
        }
    }
    if (ImGui::IsItemHovered()) wrapTooltip(380 * gUi, tr("Writes the memories of the lists that are switched on. CHIRP and SDR# files keep repeater offsets, tones and the demodulator; OnAir files keep everything."));
}

// the preview popup; call every frame
void importPopup(MemoryBank& b) {
    if (s.open) { ImGui::OpenPopup("##memimp"); s.open = false; }
    ImGui::SetNextWindowSize(ImVec2(640 * gUi, 440 * gUi), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("##memimp", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    ImGui::Text("%s: %s", tr("Import"), s.prevFile.c_str());
    ImGui::TextDisabled("%s: %s    %zu %s, %zu %s", tr("Format"), dect2::listFormatName(s.prev.format), s.prev.fresh, tr("new"), s.prev.duplicates, tr("already there"));
    if (s.prev.rep.skipped) ImGui::TextDisabled("%zu %s", s.prev.rep.skipped, tr("lines had no frequency and are left out"));
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::BeginCombo(tr("Same frequency and mode"), ruleName(s.rule))) {
        for (int i = 0; i < 4; i++) if (ImGui::Selectable(ruleName(i), s.rule == i)) s.rule = i;
        ImGui::EndCombo();
    }
    if (ImGui::SmallButton(tr("Tick all"))) for (auto& r : s.prev.rows) r.take = true;
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Untick the ones I have"))) for (auto& r : s.prev.rows) r.take = !r.duplicate;
    if (ImGui::BeginTable("##impt", 5, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("##take", ImGuiTableColumnFlags_WidthFixed, 26 * gUi);
        ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthStretch, 3.f);
        ImGui::TableSetupColumn(tr("Frequency"), ImGuiTableColumnFlags_WidthFixed, 110 * gUi);
        ImGui::TableSetupColumn(tr("List"), ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn(tr("Status"), ImGuiTableColumnFlags_WidthFixed, 90 * gUi);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < s.prev.rows.size(); i++) {
            dect2::ImportRow& r = s.prev.rows[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Checkbox("##t", &r.take);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.m.name.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(memui::freqReadout(r.m.freqHz).c_str());
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", r.m.group.c_str());
            ImGui::TableNextColumn(); if (r.duplicate) ImGui::TextDisabled("%s", tr("have it")); else ImGui::TextUnformatted(tr("new"));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    size_t taken = 0;
    for (const auto& r : s.prev.rows) taken += r.take;
    char lab[80];
    snprintf(lab, sizeof lab, "%s (%zu)###impgo", tr("Import"), taken);
    if (ImGui::Button(lab) && taken) {
        const dect2::ImportResult res = dect2::applyImport(b, s.prev, kRules[s.rule]);
        s.lists.sync(b);
        saveLists();
        memoryChanged();
        char bf[200];
        snprintf(bf, sizeof bf, tr("Added %zu, merged %zu, left out %zu."), res.added, res.merged, res.left);
        say(bf);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// the "My lists" section: switch lists on and off, order them, rename, add a built-in list
void listsSection(MemoryBank& b) {
    ensure();
    s.lists.sync(b);
    if (!ImGui::CollapsingHeader(trw("My lists"))) return;
    ImGui::TextDisabled("%s", tr("A list is a group of memories. Switched-off lists are hidden from the panel and the quick-tune bar."));
    int moveFrom = -1, moveTo = -1;
    const auto& L = s.lists.lists();
    for (size_t i = 0; i < L.size(); i++) {
        ImGui::PushID((int)i);
        bool on = L[i].enabled;
        if (ImGui::Checkbox("##on", &on)) { s.lists.setEnabled(L[i].name, on); saveLists(); }
        ImGui::SameLine();
        if (s.renaming == L[i].name) {
            ImGui::SetNextItemWidth(180 * gUi);
            if (ImGui::InputText("##rn", s.renameTo, sizeof s.renameTo, ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (s.lists.rename(b, L[i].name, s.renameTo)) { memoryChanged(); saveLists(); }
                else say(tr("That name is taken or empty."), true);
                s.renaming.clear();
            }
        } else {
            int n = 0, st = 0;
            for (const Memory& m : b.all()) if (m.group == L[i].name) { n++; st += m.favourite; }
            ImGui::Text("%s  (%d)%s", L[i].name.c_str(), n, st ? ("  *" + std::to_string(st)).c_str() : "");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Up")) && i > 0) { moveFrom = (int)i; moveTo = (int)i - 1; }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Down")) && i + 1 < L.size()) { moveFrom = (int)i; moveTo = (int)i + 1; }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Rename"))) { s.renaming = L[i].name; snprintf(s.renameTo, sizeof s.renameTo, "%s", L[i].name.c_str()); }
        ImGui::PopID();
    }
    if (moveFrom >= 0) { s.lists.move((size_t)moveFrom, (size_t)moveTo); saveLists(); }
    if (L.empty()) ImGui::TextDisabled("%s", tr("No lists yet: give memories a group, or add a built-in list below."));
    ImGui::Separator();
    // built-in lists of common services (public band plans; data/frequencies)
    const auto& bl = dect2::builtinLists();
    const auto& rg = dect2::builtinRegions();
    s.builtinSel = std::min(s.builtinSel, (int)bl.size() - 1);
    s.regionSel = std::min(s.regionSel, (int)rg.size() - 1);
    ImGui::SetNextItemWidth(190 * gUi);
    if (ImGui::BeginCombo("##biL", tr(bl[(size_t)s.builtinSel].name.c_str()))) {
        for (int i = 0; i < (int)bl.size(); i++) if (ImGui::Selectable(tr(bl[(size_t)i].name.c_str()), s.builtinSel == i)) s.builtinSel = i;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(bl[(size_t)s.builtinSel].description.c_str()));
    ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    if (ImGui::BeginCombo("##biR", rg[(size_t)s.regionSel].c_str())) {
        for (int i = 0; i < (int)rg.size(); i++) if (ImGui::Selectable(rg[(size_t)i].c_str(), s.regionSel == i)) s.regionSel = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Add built-in list..."))) {
        const std::vector<Memory> v = dect2::builtinListMemories(bl[(size_t)s.builtinSel].name, s.regionSel == 0 ? std::string() : rg[(size_t)s.regionSel]);
        if (v.empty()) say(tr("Nothing in the data for that list and region."), true);
        else beginPreview(b, dect2::writeList(v, ListFormat::OnAirJson), bl[(size_t)s.builtinSel].name, bl[(size_t)s.builtinSel].name);
    }
}

void statusLine() {
    if (!s.msg.empty() && ImGui::GetTime() - s.msgT < 12) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(s.msgBad ? pal::badRed() : pal::okGreen(), "%s", s.msg.c_str());
        ImGui::PopTextWrapPos();
    }
}

}  // namespace memui

// The quick-tune bar: the starred memories of the switched-on lists as one row of small buttons in every mode; what does not fit goes in a "..." menu.
void memoryQuickBar(App& a) {
    ensure();
    MemoryBank& b = memoryBank();
    const std::vector<size_t> q = s.lists.quickTune(b, 40);
    if (q.empty()) return;
    const float room = ImGui::GetContentRegionAvail().x;
    float used = 0;
    size_t shown = 0;
    const float more = ImGui::CalcTextSize("...").x + 24 * gUi;
    for (size_t k = 0; k < q.size(); k++) {
        const Memory& m = b.all()[q[k]];
        const std::string label = m.name.empty() ? memui::memoryTitle(m) : m.name;
        const float w = ImGui::CalcTextSize(label.c_str()).x + 16 * gUi;
        if (used + w > room - (k + 1 < q.size() ? more : 0)) break;
        used += w + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SameLine();
        ImGui::PushID((int)m.id);
        if (m.colour) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32((m.colour >> 16) & 255, (m.colour >> 8) & 255, m.colour & 255, 255));
        const bool hit = ImGui::SmallButton(label.c_str());
        if (m.colour) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", memui::memoryTitle(m).c_str());
        if (hit) {
            std::string note;
            const Memory copy = m;
            if (memoryRecallApp(a, copy, true, note)) { b.touch(copy.id, 0); memoryChanged(); }
        }
        ImGui::PopID();
        shown++;
    }
    if (shown < q.size()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("...##qmore")) ImGui::OpenPopup("##qmore");
        if (ImGui::BeginPopup("##qmore")) {
            for (size_t k = shown; k < q.size(); k++) {
                const Memory& m = b.all()[q[k]];
                ImGui::PushID((int)m.id);
                if (ImGui::MenuItem(memui::memoryTitle(m).c_str())) { std::string note; const Memory copy = m; if (memoryRecallApp(a, copy, true, note)) { b.touch(copy.id, 0); memoryChanged(); } }
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
    }
}
