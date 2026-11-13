// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The signal list of the transmit panel: choose the signal. A searchable list of every signal OnAir can send, grouped like the receiver's mode list (same headings,
// colours and one-line descriptions), a small icon per group, the chosen one highlighted; and the last signals sent as one-click buttons.
#include "tx_state.h"
#include "../ui/icons.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

using namespace txui;

namespace {

// one icon per heading of the mode list (kGroupNames order: TV, broadcast, aviation, maritime, satellite, two-way, cellular, pagers, IoT, weather, amateur digital, amateur voice, HF, general)
Ic groupIcon(int g) {
    static const Ic ic[kNumGroups] = {Ic::Tv, Ic::Radio, Ic::Antenna, Ic::Compass, Ic::Globe, Ic::Speaker, Ic::Signal, Ic::Bolt, Ic::Chip, Ic::Gauge, Ic::Wave, Ic::Speaker, Ic::Pulse, Ic::Grid, Ic::Link, Ic::Wave};
    return ic[std::max(0, std::min(kNumGroups - 1, g))];
}

// case-insensitive substring
bool contains(const char* hay, const char* needle) {
    if (!*needle) return true;
    if (!hay) return false;
    const size_t n = strlen(needle);
    for (const char* p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return true;
    }
    return false;
}

// does the row match the search words (name, description, search keys, id)?
bool matches(const ModeDef& m, const TxModeSpec& t, const char* q) {
    if (!*q) return true;
    return contains(m.name, q) || contains(tr(m.name), q) || (m.blurb && (contains(m.blurb, q) || contains(tr(m.blurb), q))) || (m.keys && contains(m.keys, q)) || contains(t.id, q) || contains(t.name, q);
}

// one row: icon, name, and the description under it; true when clicked
bool signalRow(const ModeDef& m, const TxModeSpec& t, bool sel, ImU32 gcol) {
    const float lh = ImGui::GetTextLineHeight();
    const float rowH = lh * 2.15f + 4 * gUi;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    char id[64];
    snprintf(id, sizeof id, "##sig_%s", t.id);
    const bool hit = ImGui::Selectable(id, sel, 0, ImVec2(0, rowH));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float ix = p.x + 8 * gUi;
    icons::draw(groupIcon(m.group), ImVec2(ix + lh * 0.6f, p.y + rowH * 0.5f), lh * 1.2f, gcol, dl);
    const float tx = ix + lh * 1.5f + 6 * gUi;
    const ImU32 txt = ImGui::GetColorU32(ImGuiCol_Text), dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const float maxW = ImGui::GetContentRegionAvail().x + p.x - tx - 6 * gUi;
    dl->PushClipRect(ImVec2(tx, p.y), ImVec2(tx + std::max(20.f, maxW), p.y + rowH), true);
    dl->AddText(ImVec2(tx, p.y + 2 * gUi), txt, tr(m.name));
    if (m.blurb) dl->AddText(ImVec2(tx, p.y + 2 * gUi + lh * 1.1f), dim, tr(m.blurb));
    dl->PopClipRect();
    if (sel) dl->AddRectFilled(ImVec2(p.x, p.y + 2 * gUi), ImVec2(p.x + 3 * gUi, p.y + rowH - 2 * gUi), gcol);
    return hit;
}

} // namespace

namespace txui {

// The signal list; `listH` is the height of the list itself (0: what is left)
void stepSignal(App& a, TxUi& s, bool running, float listH) {
    const TxModeSpec* cur = txModeById(s.modeId);
    ImGui::BeginDisabled(running);

    if (!s.recent.empty()) {   // the last signals: one click brings back the signal, frequency and level
        ImGui::TextDisabled("%s", tr("Recent"));
        for (size_t i = 0; i < s.recent.size(); i++) {
            const TxModeSpec* rm = txModeById(s.recent[i].id);
            if (!rm) continue;
            char lab[96];
            snprintf(lab, sizeof lab, "%s  %.3f##rec%zu", rm->name, s.recent[i].mhz, i);
            const float w = ImGui::CalcTextSize(lab).x + 2 * ImGui::GetStyle().FramePadding.x;
            // stay on the line only when the button fits in what is left of it (the cursor is already on the next line after a button)
            const float left = ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x - ImGui::GetItemRectMax().x - ImGui::GetStyle().ItemSpacing.x;
            if (i > 0 && left > w) ImGui::SameLine();
            if (ImGui::SmallButton(lab)) {
                // the list row of the mode picks the engine family and sub-protocol; search the table for it
                for (const auto& m : allModes()) {
                    const TxModeSpec* t = txModeForList(m.family, m.subId, s.dvbVariant);
                    if (t == rm) { txSelectMode(a, m.family, m.subId); break; }
                }
                s.freqMhz = s.recent[i].mhz;
                s.gainDb = s.recent[i].gain;
                s.gainSet = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("%s at %.6f MHz, level %.1f dB"), rm->name, s.recent[i].mhz, (double)s.recent[i].gain);
        }
        ImGui::Spacing();
    }

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##txsearch", tr("Search signals..."), s.search, sizeof s.search);
    if (listH <= 0) listH = std::max(120.f * gUi, ImGui::GetContentRegionAvail().y - 4 * gUi);
    if (ImGui::BeginChild("##txsignals", ImVec2(0, listH), ImGuiChildFlags_None)) {
        int shown = 0;
        const char* seen[200];
        int nseen = 0;
        for (int g = 0; g < kNumGroups; g++) {
            bool head = false;
            for (const auto& m : allModes()) {
                if (m.group != g) continue;
                const TxModeSpec* t = txModeForList(m.family, m.subId, s.dvbVariant);
                if (!t || !matches(m, *t, s.search)) continue;
                bool dup = false;
                for (int k = 0; k < nseen; k++) if (seen[k] == t->id) dup = true;   // two list rows may share one generator (DVB-T2 / T "auto")
                if (dup) continue;
                if (nseen < 200) seen[nseen++] = t->id;
                if (!head) {
                    head = true;
                    ImGui::Spacing();
                    const ImVec2 hp = ImGui::GetCursorScreenPos();
                    ImGui::GetWindowDrawList()->AddRectFilled(hp, ImVec2(hp.x + 3 * gUi, hp.y + ImGui::GetTextLineHeight()), kGroupCols[g]);
                    ImGui::SetCursorScreenPos(ImVec2(hp.x + 8 * gUi, hp.y));
                    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kGroupCols[g]), "%s", tr(kGroupNames[g]));
                }
                if (signalRow(m, *t, cur == t, kGroupCols[g])) txSelectMode(a, m.family, m.subId);
                shown++;
            }
        }
        if (!shown) ImGui::TextDisabled("%s", tr("Nothing matches this search."));
    }
    ImGui::EndChild();
    if (!cur) {
        const std::string why = txRefusalReason(txEngineStd(s.family));
        banner(Note::Info, why.empty() ? tr("this mode has no transmit generator") : tr(why.c_str()));
    }
    ImGui::EndDisabled();
}

} // namespace txui
