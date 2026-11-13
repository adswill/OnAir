// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Band hopping for FT8 / FT4 / FT2 / WSPR on one narrow radio: the plan (bands per UTC hour, coordinated or in turn) and the retune at each
// 2-minute slot. Core: dect2/hfdig/hfdig_hop.h; the dial frequencies are the multiband ones (WSJT-X defaults).
#include "app.h"
#include "dect2/hfdig/hfdig_ftx.h"
#include "dect2/hfdig/hfdig_hop.h"
#include "dect2/hfdig/hfdig_multiband.h"
#include "i18n/i18n.h"
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

void hfdigHopTick(App& a);
void hfdigHopPanel(App& a, int mode);

namespace {
struct Hop {
    bool loaded = false, on = false;
    dect2::HopPlan plan;
    int mode = 3;            // the ftx mode (0 FT8, 1 FT4, 2 FT2, 3 WSPR, 4 JT65 ...): the built-in dial frequencies exist for 0 .. 3, the user's own for any
    int lastBand = -2;
    int64_t lastSlot = 0;
    std::string note;
};
Hop H;
void load() {
    if (H.loaded) return;
    H.loaded = true;
    plat::Prefs& p = plat::prefs();
    H.on = p.getB("hfdigHopOn", false);
    H.plan = p.has("hfdigHopPlan") ? dect2::hopFromText(p.getS("hfdigHopPlan", "")) : dect2::HopPlan::allHours(0x3FF);
    H.mode = (int)p.getI("hfdigHopMode", 3);
}
void save() { plat::prefs().setS("hfdigHopPlan", dect2::hopToText(H.plan)); plat::prefs().setB("hfdigHopOn", H.on); plat::prefs().setI("hfdigHopMode", H.mode); }
void tuneTo(App& a, double hz) {
    if (std::fabs(a.freqMhz * 1e6 - hz) <= 1) return;
    a.freqMhz = hz / 1e6;
    a.tune.centerHz = a.freqMhz * 1e6;
    a.engine.retuneReset(a.tune);
    a.peak.clear();
}
}  // namespace

void hfdigHopTick(App& a) {
    load();
    if (!H.on || !a.engine.running() || a.rx.standard != 26) return;
    const int64_t now = (int64_t)time(nullptr);
    const int64_t slot = now / H.plan.slotSec;
    if (slot == H.lastSlot) return;
    H.lastSlot = slot;
    const int b = dect2::hopBand(H.plan, now, H.mode);
    if (b < 0) { H.note = tr("no band on this hour"); return; }
    const double hz = dect2::hopDialHz(H.plan, b, H.mode);
    if (hz <= 0) { H.note = dect2::hopBandName(H.plan, b) + tr(": no dial frequency for this mode"); return; }
    tuneTo(a, hz);
    H.lastBand = b;
    H.note = std::string(tr("now ")) + dect2::hopBandName(H.plan, b);
}

void hfdigHopPanel(App& a, int mode) {
    load();
    if (mode >= 0 && mode < dect2::kFtxModes && mode != H.mode) { H.mode = mode; save(); }
    if (!ImGui::CollapsingHeader(trw("Band hopping##hfhop"))) return;
    if (ImGui::Checkbox(tr("Hop bands every 2 minutes"), &H.on)) { save(); H.lastSlot = 0; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("For one narrow radio: at the start of each 2-minute slot the dial moves to the next band switched on for this UTC hour. The radio follows when it is under OnAir's control."));
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Coordinated (the same band as other stations)"), &H.plan.coordinated)) save();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The 20-minute cycle 160, 80, 60, 40, 30, 20, 17, 15, 12, 10 m, one band per 2-minute slot from the hour, as WSJT-X's coordinated hopping. A band you switched off gives its slot to the next one. Your own frequencies are not part of that cycle: with this on they are used only in an hour where no standard band is ticked; switch it off to hop to them in turn."));
    if (!H.note.empty() && H.on) { ImGui::SameLine(); ImGui::TextDisabled("%s, %s %s", H.note.c_str(), tr("mode"), dect2::ftxModeName(H.mode)); }
    // the plan: one row per UTC hour, one column per band
    const int nb = dect2::kMbBands;
    const int nu = (int)H.plan.user.size();
    if (!ImGui::BeginTable("##hophrs", nb + nu + 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, 220 * gUi))) return;
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("UTC");
    for (int b = 0; b < nb; b++) ImGui::TableSetupColumn(dect2::mbBand(b).name);
    std::vector<std::string> uname((size_t)nu);
    for (int i = 0; i < nu; i++) {
        char nm[32]; snprintf(nm, sizeof nm, "%.4f", H.plan.user[(size_t)i].hz / 1e6);
        uname[(size_t)i] = H.plan.user[(size_t)i].label.empty() ? std::string(nm) : H.plan.user[(size_t)i].label.substr(0, 10);
        ImGui::TableSetupColumn((uname[(size_t)i] + "##u" + std::to_string(i)).c_str());
    }
    ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();
    const int cur = (int)((time(nullptr) % 86400) / 3600);
    bool ch = false;
    for (int h = 0; h < 24; h++) {
        ImGui::TableNextRow();
        if (h == cur) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(50, 70, 50, 255));
        ImGui::TableNextColumn(); ImGui::Text("%02d", h);
        for (int b = 0; b < nb; b++) {
            ImGui::TableNextColumn();
            bool on = H.plan.hours[(size_t)h] >> b & 1;
            ImGui::PushID(h * 32 + b);
            if (ImGui::Checkbox("##b", &on)) { H.plan.hours[(size_t)h] = on ? H.plan.hours[(size_t)h] | (1u << b) : H.plan.hours[(size_t)h] & ~(1u << b); ch = true; }
            ImGui::PopID();
        }
        for (int i = 0; i < nu; i++) {
            ImGui::TableNextColumn();
            const int b = dect2::kHopUserBase + i;
            bool on = H.plan.hours[(size_t)h] >> b & 1;
            ImGui::PushID(h * 32 + b);
            if (ImGui::Checkbox("##b", &on)) { H.plan.hours[(size_t)h] = on ? H.plan.hours[(size_t)h] | (1u << b) : H.plan.hours[(size_t)h] & ~(1u << b); ch = true; }
            ImGui::PopID();
        }
        ImGui::TableNextColumn();
        ImGui::PushID(h);
        if (ImGui::SmallButton(tr("all"))) { H.plan.hours[(size_t)h] = 0x3FF; for (int i = 0; i < nu; i++) H.plan.hours[(size_t)h] |= 1u << (dect2::kHopUserBase + i); ch = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("copy down")) && h < 23) { for (int k = h + 1; k < 24; k++) H.plan.hours[(size_t)k] = H.plan.hours[(size_t)h]; ch = true; }
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (ch) { save(); H.lastSlot = 0; }
    // the user's own frequencies (a DXpedition, a net): added here, hopped to in the plan above, tuned, deleted
    ImGui::Separator();
    ImGui::TextUnformatted(tr("Your own frequencies"));
    static double addMhz = 14.091;
    static int addMode = -1;
    if (addMode < 0) addMode = H.mode >= 0 && H.mode < dect2::kFtxModes ? H.mode : 0;
    static char addLabel[40] = "";
    static std::string addMsg, addWarn;
    ImGui::SetNextItemWidth(110 * gUi);
    ImGui::InputDouble("##hopaddf", &addMhz, 0, 0, "%.4f MHz");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The dial frequency in MHz (upper sideband), for example 14.0910 for a DXpedition. Any frequency from 100 kHz to 1.3 GHz is accepted; one outside the amateur bands or away from the usual dial only gets a warning."));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    if (ImGui::BeginCombo("##hopaddm", dect2::ftxModeName(addMode))) {
        for (int m = 0; m < dect2::kFtxModes; m++) if (ImGui::Selectable(dect2::ftxModeName(m), m == addMode)) addMode = m;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140 * gUi);
    ImGui::InputTextWithHint("##hopaddl", tr("label (optional)"), addLabel, sizeof addLabel);
    ImGui::SameLine();
    if (ImGui::Button(tr("Add frequency"))) {
        dect2::HopUserFreq f; f.hz = addMhz * 1e6; f.mode = addMode; f.label = addLabel; f.enabled = true;
        addMsg = dect2::hopAddUser(H.plan, f);
        addWarn = addMsg.empty() ? dect2::hopUserWarning(f.hz, f.mode) : "";
        if (addMsg.empty()) { addLabel[0] = 0; save(); H.lastSlot = 0; }
    }
    if (!addMsg.empty()) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1), "%s", addMsg.c_str());
    if (!addWarn.empty()) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1), "%s", addWarn.c_str());
    int del = -1;
    for (int i = 0; i < (int)H.plan.user.size(); i++) {
        dect2::HopUserFreq& u = H.plan.user[(size_t)i];
        ImGui::PushID(1000 + i);
        if (ImGui::Checkbox("##en", &u.enabled)) { save(); H.lastSlot = 0; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Hop to this frequency (when its mode is the one shown here and the hour is ticked in the table)."));
        ImGui::SameLine();
        ImGui::Text("%.4f MHz  %s  %s", u.hz / 1e6, dect2::ftxModeName(u.mode), u.label.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Tune"))) { tuneTo(a, u.hz); H.note = std::string(tr("now ")) + dect2::hopBandName(H.plan, dect2::kHopUserBase + i); }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Delete"))) del = i;
        const std::string w = dect2::hopUserWarning(u.hz, u.mode);
        if (!w.empty()) { ImGui::SameLine(); ImGui::TextDisabled("(!)"); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", w.c_str()); }
        ImGui::PopID();
    }
    if (del >= 0) { dect2::hopRemoveUser(H.plan, del); save(); H.lastSlot = 0; }
}
