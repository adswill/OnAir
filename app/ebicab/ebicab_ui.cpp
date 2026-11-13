// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// EBICab / Eurobalise screens: the Eurobalise telegrams heard (SUBSET-036 coding, the SUBSET-026 header fields), the balises, and the FSK bursts
// that gave no telegram (length and first bits; EBICab 700 / 900 formats are not published). "Needs a user test": no real recording was available.
#include "app.h"
#include "i18n/i18n.h"
#include "dect2/common/time_fmt.h"
#include "dect2/ebicab/ebicab_rx.h"
#include "dect2/ebicab/ebicab_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct State { int view = 0; };
State S;

bool live(const App& a) { return a.rx.standard == 208; }   // the engine reports its standard code minus one

void banner(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s", tr("Needs a user test"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Written from the public Eurobalise specification (UNISIG SUBSET-036) only; no real recording was available. EBICab 700 and 900 telegram formats are not public and are not decoded: such bursts are listed with their length."));
    flowNext(8 * gUi);
    ImGui::TextDisabled("%s", tr("Radio: HackRF or another receiver that reaches 4.2 MHz at 4 Msps or more, antenna: a small loop near the track-side antenna."));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The up-link is a magnetic field around 4.234 MHz (FSK 3.951 / 4.516 MHz, 564.48 kbit/s). It exists only while a train's 27.095 MHz tele-powering field is over the balise. The radio is tuned 1 MHz higher so the signal stays clear of the DC spike."));
    flowNext(10 * gUi);
    char cl[40]; snprintf(cl, sizeof cl, "%s##ebi", tr("Clear"));
    if (ImGui::Button(cl)) a.engine.ebicab().clearResults();
    flowEnd();
}

void telegramsView(App& a) {
    const auto& tg = a.rx.ebicab.telegrams;
    if (!ImGui::BeginTable("##ebit", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn(tr("Length")); ImGui::TableSetupColumn("NID_C"); ImGui::TableSetupColumn("NID_BG"); ImGui::TableSetupColumn("N_PIG");
    ImGui::TableSetupColumn("N_TOTAL"); ImGui::TableSetupColumn("M_VERSION"); ImGui::TableSetupColumn(tr("Copies")); ImGui::TableSetupColumn(tr("Detail"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    for (const auto& t : tg) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(t.utc).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%s", t.longFmt ? "1023" : "341");
        if (t.header.valid) {
            ImGui::TableNextColumn(); ImGui::Text("%d", t.header.nidC);
            ImGui::TableNextColumn(); ImGui::Text("%d", t.header.nidBg);
            ImGui::TableNextColumn(); ImGui::Text("%d", t.header.nPig);
            ImGui::TableNextColumn(); ImGui::Text("%d", t.header.nTotal);
            ImGui::TableNextColumn(); ImGui::Text("%d.%d", t.header.mVersion >> 4, t.header.mVersion & 15);
        } else for (int i = 0; i < 5; i++) { ImGui::TableNextColumn(); ImGui::TextDisabled("-"); }
        ImGui::TableNextColumn(); ImGui::Text("%d", t.repeats);
        ImGui::TableNextColumn();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s%s%s", t.inverted ? "inverted  " : "", t.header.qUpdown ? "up-link  " : "down-link  ", t.hex.substr(0, 40).c_str());
        for (const auto& pk : t.packets) ImGui::Text("%d: %s", pk.nid, pk.text.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t.hex.c_str());
    }
    ImGui::PopFont();
    ImGui::EndTable();
    if (tg.empty()) ImGui::TextDisabled("%s", tr("no telegrams yet"));
}

void balisesView(App& a) {
    const auto& bl = a.rx.ebicab.balises;
    if (!ImGui::BeginTable("##ebib", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("NID_C"); ImGui::TableSetupColumn("NID_BG"); ImGui::TableSetupColumn(tr("Balise in group")); ImGui::TableSetupColumn(tr("Group size"));
    ImGui::TableSetupColumn(tr("Last heard")); ImGui::TableSetupColumn(tr("Count"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    for (const auto& b : bl) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%d", b.nidC);
        ImGui::TableNextColumn(); ImGui::Text("%d", b.nidBg);
        ImGui::TableNextColumn(); ImGui::Text("%d", b.nPig);
        ImGui::TableNextColumn(); ImGui::Text("%d", b.nTotal + 1);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(b.lastSeen).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)b.count);
    }
    ImGui::PopFont();
    ImGui::EndTable();
    if (bl.empty()) ImGui::TextDisabled("%s", tr("no balises yet"));
}

void burstsView(App& a) {
    const auto& bs = a.rx.ebicab.burstLog;
    if (!ImGui::BeginTable("##ebiu", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn(tr("Bits")); ImGui::TableSetupColumn(tr("Duration")); ImGui::TableSetupColumn("dB"); ImGui::TableSetupColumn(tr("First bits (hex) and hint"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    for (const auto& b : bs) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(b.utc).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%d", b.bits);
        ImGui::TableNextColumn(); ImGui::Text("%.0f us", b.durationUs);
        ImGui::TableNextColumn(); ImGui::Text("%.0f", b.snrDb);
        ImGui::TableNextColumn(); ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", b.head.substr(0, 48).c_str()); if (!b.hint.empty()) ImGui::TextUnformatted(tr(b.hint.c_str())); ImGui::PopTextWrapPos();
    }
    ImGui::PopFont();
    ImGui::EndTable();
    if (bs.empty()) ImGui::TextDisabled("%s", tr("no other bursts"));
}

void tab(App& a) {
    banner(a);
    ImGui::Spacing();
    subNav("ebiv", S.view, {"Telegrams", "Balises", "Other bursts"});
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    if (S.view == 0) telegramsView(a); else if (S.view == 1) balisesView(a); else burstsView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const auto& bl = a.rx.ebicab.balises;
    if (!ImGui::BeginTable("##ebil", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn(tr("Balise")); ImGui::TableSetupColumn(tr("Heard"), ImGuiTableColumnFlags_WidthStretch);
    for (const auto& b : bl) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::Text("%d/%d/%d", b.nidC, b.nidBg, b.nPig); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s, %llux", dect2::timefmt::clock(b.lastSeen).c_str(), (unsigned long long)b.count);
    }
    ImGui::EndTable();
    if (bl.empty()) ImGui::TextDisabled("%s", tr("no balises yet"));
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::EbicabTelemetry& t = a.rx.ebicab;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv(tr("state"), "%s", dect2::ebicabSummary(t).c_str());
    kv(tr("telegrams"), "%llu (%llu long, %llu short, %llu inverted)", (unsigned long long)t.blocksOk, (unsigned long long)t.longTelegrams, (unsigned long long)t.shortTelegrams, (unsigned long long)t.invertedTelegrams);
    kv(tr("bursts"), "%llu (%llu %s)", (unsigned long long)t.bursts, (unsigned long long)t.blocksBad, tr("without a telegram"));
    if (t.bitRate > 0) kv(tr("data rate"), "%.1f kbit/s (564.5 nominal)", t.bitRate / 1e3);
    kv(tr("carrier offset"), "%+.0f kHz", t.cfoHz / 1e3);
    kv(tr("input"), "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kv(tr("signal time"), "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Burst"), !on ? 0 : a.rx.ebicab.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp(tr("Telegram"), !on ? 0 : a.rx.ebicab.dataValid ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::ebicabSummary(a.rx.ebicab).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "EBICab / Eurobalise";
    if (live(a)) l2 = dect2::ebicabSummary(a.rx.ebicab);
}

} // namespace

extern const ModeUi kEbicabUi;
const ModeUi kEbicabUi = {
    .sideTitle = "BALISES",
    .tabName = "Balise",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = nullptr,
};
