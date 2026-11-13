// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The watch list view and alerts of the voice modes; see watch_ui.h.
#include "watch_ui.h"
#include "app.h"
#include "i18n/i18n.h"
#include "ui/ui_common.h"
#include "dect2/common/time_fmt.h"
#include "dect2/session/record_rules.h"
#include "../panels/automation_internal.h"
#include "platform/platform.h"
#include <cstdio>
#include <string>

namespace watchui {
namespace {

using dect2::voice::WatchEntry;

struct State {
    bool loaded = false;
    uint64_t savedGen = 0;
    char match[96] = "", name[64] = "";
    bool allModes = false;
    std::string note;
};
State S;

void save() {
    std::string t = dect2::voice::watch().saveText();
    for (auto& c : t) if (c == '\n') c = '\x1e';
    plat::prefs().setS("voiceWatch", t);
    plat::prefs().flush();
}

std::string clockOf(int64_t wall) { return wall ? dect2::timefmt::clock(wall) : "-"; }

const char* modeTitle(const std::string& m) {
    if (m.empty()) return "";
    const char* n = dect2::sess::voicebridge::modeName(m);
    return n ? n : m.c_str();
}

}  // namespace

void tick(App& a) {
    dect2::voice::Watch& w = dect2::voice::watch();
    if (!S.loaded) {
        S.loaded = true;
        std::string t = plat::prefs().getS("voiceWatch", "");
        for (auto& c : t) if (c == '\x1e') c = '\n';
        if (!t.empty()) w.loadText(t);
        S.savedGen = w.gen();
    }
    if (w.gen() != S.savedGen) {   // an edit: keep the record rules and the saved copy in step
        w.syncRecord(dect2::sess::rules());
        save();
        S.savedGen = w.gen();
    }
    const std::vector<dect2::voice::WatchEvent> fresh = w.poll();
    if (fresh.empty()) return;
    const std::vector<WatchEntry> list = w.list();
    int sent = 0;
    for (const auto& ev : fresh) {
        const WatchEntry* e = nullptr;
        for (const auto& x : list) if (x.id == ev.entry) e = &x;
        if (!e || !e->alert) continue;
        if (++sent > 3) continue;   // a busy talkgroup must not bury the desktop
        automationapp::alertNow(a, std::string(tr("Watch")) + ": " + e->title() + (ev.emergency ? std::string(" ") + tr("EMERGENCY") : ""), ev.text + (ev.encrypted ? std::string(" (") + tr("encrypted") + ")" : ""), e->sound && sent == 1);
    }
}

void watchView(App& a, const char* mode, const std::function<void(const WatchEntry&)>& extra) {
    dect2::voice::Watch& w = dect2::voice::watch();
    ImGui::TextWrapped("%s", tr("Follow talkgroups, radio IDs or callsigns. Alerts and recording are off until you tick them for an entry. Encrypted calls are listed but never recorded."));
    // add row
    ImGui::SetNextItemWidth(200 * gUi);
    ImGui::InputTextWithHint("##wlmatch", tr("TG 91, 3120123, 100-199, LY1ABC, LY*"), S.match, sizeof S.match);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130 * gUi);
    ImGui::InputTextWithHint("##wlname", tr("Name (optional)"), S.name, sizeof S.name);
    ImGui::SameLine();
    ImGui::Checkbox(tr("All voice modes##wlall"), &S.allModes);
    ImGui::SameLine();
    if (ImGui::Button(tr("Watch##wladd")) && S.match[0]) {
        WatchEntry e; e.match = S.match; e.name = S.name; e.mode = S.allModes ? "" : mode;
        if (w.add(e)) { S.match[0] = 0; S.name[0] = 0; }
    }
    // the list
    const std::vector<WatchEntry> list = w.list();
    if (list.empty()) ImGui::TextDisabled("%s", tr("Nothing watched yet."));
    uint32_t del = 0;
    if (!list.empty() && ImGui::BeginTable("##wllist", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn(tr("Watching"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Mode"));
        ImGui::TableSetupColumn(tr("Notify"));
        ImGui::TableSetupColumn(tr("Beep"));
        ImGui::TableSetupColumn(tr("Record"));
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (const WatchEntry& e0 : list) {
            WatchEntry e = e0;
            bool ch = false;
            ImGui::PushID((int)e.id);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.title().c_str()); if (!e.name.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", e.match.c_str()); }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.mode.empty() ? tr("all") : modeTitle(e.mode));
            ImGui::TableNextColumn(); ch |= ImGui::Checkbox("##wlnot", &e.alert);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A desktop notification when a call starts (off by default)"));
            ImGui::TableNextColumn(); ImGui::BeginDisabled(!e.alert); ch |= ImGui::Checkbox("##wlbeep", &e.sound); ImGui::EndDisabled();
            ImGui::TableNextColumn(); ch |= ImGui::Checkbox("##wlrec", &e.record);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Save the audio of its calls to the recordings folder through the auto-record rules (this also switches auto-record on). Encrypted calls are never recorded. You are responsible for the law of your country."));
            ImGui::TableNextColumn(); if (ImGui::SmallButton(tr("Remove##wldel"))) del = e.id;
            if (ch) w.update(e);
            ImGui::PopID();
            if (extra) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::PushID((int)e.id + 100000);
                extra(e0);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    if (del) w.remove(del);
    // history: what was heard of the watched entries (this program's monitors keep the last calls of each mode)
    ImGui::Separator();
    ImGui::TextUnformatted(tr("History"));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Copy as CSV##wlcsv"))) {
        std::string o = "time,mode,watch,source,target,seconds,emergency,encrypted,recorded\n";
        for (const WatchEntry& e : list)
            for (const auto& h : w.history(e.id)) {
                const auto& c = h.second;
                char b[96]; snprintf(b, sizeof b, ",%.0f,%d,%d,%d\n", c.seconds(), c.info.emergency ? 1 : 0, c.info.encrypted ? 1 : 0, c.file.empty() ? 0 : 1);
                o += clockOf(c.wallStart) + "," + h.first + ",\"" + e.title() + "\",\"" + dect2::voice::sourceText(c.info) + "\",\"" + dect2::voice::targetText(c.info) + "\"" + b;
            }
        ImGui::SetClipboardText(o.c_str());
    }
    struct Row { const WatchEntry* e; std::string mode; dect2::voice::CallEntry c; };
    std::vector<Row> rows;
    for (const WatchEntry& e : list) for (auto& h : w.history(e.id, 100)) rows.push_back({&e, h.first, h.second});
    std::stable_sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.c.wallStart > y.c.wallStart; });
    if (rows.size() > 200) rows.resize(200);
    if (rows.empty()) { ImGui::TextDisabled("%s", tr("No calls of the watched entries heard yet.")); return; }
    if (ImGui::BeginTable("##wlhist", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, std::max(120.f * gUi, ImGui::GetContentRegionAvail().y)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Time")); ImGui::TableSetupColumn(tr("Watching")); ImGui::TableSetupColumn(tr("Mode"));
        ImGui::TableSetupColumn(tr("From")); ImGui::TableSetupColumn(tr("To")); ImGui::TableSetupColumn(tr("Sec"));
        ImGui::TableSetupColumn(tr("Flags")); ImGui::TableSetupColumn(tr("Recording"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const Row& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clockOf(r.c.wallStart).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.e->title().c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(modeTitle(r.mode));
            ImGui::TableNextColumn(); ImGui::TextUnformatted((dect2::voice::sourceText(r.c.info) + (r.c.srcLabel.empty() ? "" : " " + r.c.srcLabel)).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted((dect2::voice::targetText(r.c.info) + (r.c.label.empty() ? "" : " " + r.c.label)).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.0f%s", r.c.seconds(), r.c.active ? "+" : "");
            ImGui::TableNextColumn(); { std::string f; if (r.c.info.emergency) f += std::string(tr("EMERG")) + " "; if (r.c.info.encrypted) f += tr("ENC"); ImGui::TextUnformatted(f.empty() ? "-" : f.c_str()); }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.c.file.empty() ? "-" : r.c.file.c_str());
        }
        ImGui::EndTable();
    }
    (void)a;
}

}  // namespace watchui
