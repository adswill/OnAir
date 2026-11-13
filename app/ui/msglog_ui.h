// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Save..." and "Auto-save to file" for a message tab: writes the rows as a .txt / .log text (dect2/common/msg_log.h), time in the
// Time display setting. Auto-save appends each new message to a file chosen by the user (default in the data folder, "messages/<id>.log"),
// with a new file each day (name-YYYY-MM-DD.log).
// Use: msglog_ui::bar("pager", "Pager", rowsOldestFirst);   // in the tab's button row, once per frame
#pragma once
#include "../app.h"
#include "dect2/common/msg_log.h"
#include <algorithm>
#include <functional>
#include <map>
#include <unordered_set>

namespace msglog_ui {

struct State {
    bool loaded = false, autosave = false;
    dect2::msglog::AutoSaver saver;
    std::unordered_set<uint64_t> seen;   // messages already appended (so a list that is trimmed or redrawn never writes twice)
    std::string note;
};

inline uint64_t rowKey(const dect2::msglog::Row& r) {
    uint64_t h = 1469598103934665603ull ^ (uint64_t)r.epoch;
    auto mix = [&](const std::string& s) { for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; } h ^= 0xff; h *= 1099511628211ull; };
    mix(r.mode); mix(r.address); mix(r.text);
    h ^= (uint64_t)(r.freqHz / 100.0); h *= 1099511628211ull;
    return h;
}

inline State& state(const std::string& id) { static std::map<std::string, State> m; return m[id]; }

inline void load(State& s, const std::string& id) {
    if (s.loaded) return;
    s.loaded = true;
    s.autosave = plat::prefs().getB(("msglogAuto_" + id).c_str(), false);
    s.saver.setBase(plat::prefs().getS(("msglogPath_" + id).c_str(), plat::dataDir() + "/messages/" + id + ".log"));
}

// rows: oldest first. title: shown in the file dialog name only.
inline void bar(const char* id, const char* title, const std::vector<dect2::msglog::Row>& rows) {
    State& s = state(id);
    load(s, id);
    const dect2::timefmt::Zone z = dect2::timefmt::zone();
    const bool lab = dect2::timefmt::label();
    // auto-save first, so a click on the box does not skip the messages of this frame
    if (s.autosave) {
        std::string err;
        for (const auto& r : rows) {
            const uint64_t k = rowKey(r);
            if (!s.seen.insert(k).second) continue;
            if (!s.saver.append(r, z, lab, err)) { s.note = err; s.autosave = false; break; }
        }
        if (s.seen.size() > 20000) s.seen.clear();   // a very long session: forget (rows still in the list would be written again once)
    }
    ImGui::PushID(id);
    if (ImGui::Button(tr("Save..."))) {
        const std::string p = saveFileDialog((std::string(title) + "-messages.txt").c_str());
        if (!p.empty()) {
            std::string err;
            s.note = dect2::msglog::saveAll(p, rows, z, lab, err) ? std::string(tr("Saved")) + ": " + p : err;
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Save the messages in the list to a text file (.txt or .log): time, mode, frequency, address and text. The time follows View > Time display."));
    sameLineIf(ImGui::CalcTextSize(tr("Auto-save to file")).x + 36 * gUi, -1.f);   // wraps in a narrow pane (the Simple views)
    if (ImGui::Checkbox(tr("Auto-save to file"), &s.autosave)) {
        plat::prefs().setB((std::string("msglogAuto_") + id).c_str(), s.autosave);
        if (s.autosave) { s.seen.clear(); for (const auto& r : rows) s.seen.insert(rowKey(r)); s.note.clear(); }   // only messages from now on
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("Append each new message to %s (a new file each day: name-YYYY-MM-DD)."), s.saver.base().c_str());
    sameLineIf(ImGui::CalcTextSize(tr("File...")).x + 12 * gUi, -1.f);
    if (ImGui::SmallButton(tr("File..."))) {
        const std::string p = saveFileDialog("messages.log");
        if (!p.empty()) { s.saver.setBase(p); plat::prefs().setS((std::string("msglogPath_") + id).c_str(), p); }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.saver.base().c_str());
    ImGui::PopID();
    if (!s.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", s.note.c_str()); }
}

} // namespace msglog_ui
