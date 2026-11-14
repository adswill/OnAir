// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Robust Packet (RPR) screens (receive only, physical layer): the frequency presets, then four views: the signal (comb, speed level), the bursts (time,
// length, level, SNR, the first demodulated bits), the symbols (the newest phase steps of the first carriers) and the events.
// The channel coding and the AX.25 framing are not public / not decoded, so there is no text (docs/rpr.md).
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "gfx/gfx.h"
#include "i18n/i18n.h"
#include "dect2/rpr/rpr_rx.h"
#include "dect2/rpr/rpr_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct State { int view = 0; } S;

bool live(const App& a) { return a.rx.standard == 900; }   // the engine reports its standard code minus one

struct Preset { const char* name; double mhz; };
// the frequencies the sigidwiki page lists for RPR (USB dial); 10147.3 kHz is the world-wide one
const Preset kPresets[] = {{"80 m", 3.610}, {"40 m", 7.0473}, {"30 m", 10.1473}, {"20 m", 14.1033}};

void presets(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s", tr("Physical layer only, needs a recording"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Robust Packet: the carriers, the 50 baud clock, the speed level (D-BPSK or D-QPSK) and the demodulated symbols are shown. The convolutional code polynomials, the interleaver, the scrambler, the callsign compression and the packet types are not public, so no AX.25 frames or text are produced until a recording lets them be worked out."));
    flowNext(8 * gUi);
    ImGui::TextDisabled("%s", tr("Band"));
    flowNext(6 * gUi);
    for (size_t i = 0; i < sizeof kPresets / sizeof kPresets[0]; i++) {
        const bool here = std::fabs(a.freqMhz - kPresets[i].mhz) < 0.002;
        if (here) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.45f));
        char b[40]; snprintf(b, sizeof b, "%s##rpr%zu", kPresets[i].name, i);
        if (ImGui::Button(b)) tuneFreq(a, kPresets[i].mhz);
        if (here) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.4f MHz", kPresets[i].mhz);
        flowNext(4 * gUi);
    }
    ImGui::TextDisabled("|"); flowNext(8 * gUi);
    char cl[40]; snprintf(cl, sizeof cl, "%s##rprclr", tr("Clear"));
    if (ImGui::Button(cl)) a.engine.rpr().clearResults();
    flowEnd();
}

void signalView(App& a) {
    const dect2::RprTelemetry& t = a.rx.rpr;
    auto kv = [&](const char* k, const char* fmt, auto... x) { ImGui::TextDisabled("%s", k); kvColumn(150 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, x...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv(tr("state"), "%s", dect2::rprSummary(t).c_str());
    if (t.state == 0) { ImGui::TextDisabled("%s", tr("no signal yet")); return; }
    kv(tr("centre"), "%.0f Hz (%+.0f Hz %s 1500 Hz)", t.centreHz, t.cfoHz, tr("from"));
    kv(tr("comb contrast"), "%.0f dB", t.combDb);
    kv(tr("symbol rate"), "%s", "50 baud, 8 carriers 60 Hz apart");
    if (t.level) kv(tr("speed level"), "%d: %s, code rate %s (%s)", t.level, t.level == 1 ? "D-BPSK" : "D-QPSK", t.level == 1 ? "1/2" : "3/4", tr("from the published description"));
    for (size_t i = 0; i < t.carrierHz.size(); i++) {
        char k[40]; snprintf(k, sizeof k, "%s %zu", tr("carrier"), i + 1);
        kv(k, "%.1f Hz", t.carrierHz[i]);
    }
    kv(tr("bursts"), "%llu", (unsigned long long)t.blocksOk);
}

void burstsView(App& a) {
    const auto& bu = a.rx.rpr.bursts;
    if (!ImGui::BeginTable("##rprb", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn(tr("Length")); ImGui::TableSetupColumn(tr("Level")); ImGui::TableSetupColumn("SNR");
    ImGui::TableSetupColumn(tr("Symbols")); ImGui::TableSetupColumn(tr("First bits (coded)"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& b : bu) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(b.utc).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.2f s", b.lenSec);
        ImGui::TableNextColumn(); ImGui::Text("%d %s", b.level, b.level == 1 ? "D-BPSK" : "D-QPSK");
        ImGui::TableNextColumn(); ImGui::Text("%.0f dB", b.snrDb);
        ImGui::TableNextColumn(); ImGui::Text("%d", b.nSyms);
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b.bits.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
    }
    ImGui::EndTable();
    if (bu.empty()) ImGui::TextDisabled("%s", tr("no burst yet"));
}

void symbolsView(App& a) {
    const dect2::RprTelemetry& t = a.rx.rpr;
    if (t.rows.empty()) { ImGui::TextDisabled("%s", tr("no symbols yet")); return; }
    ImGui::TextDisabled("%s", tr("The phase steps of the newest burst on the first carriers (one digit per symbol; 0 / 1 in D-BPSK, 0 to 3 = steps of 90 degrees in D-QPSK):"));
    ImGui::PushFont(a.mono, 0);
    for (size_t i = 0; i < t.rows.size(); i++) {
        ImGui::TextDisabled("%zu", i + 1); ImGui::SameLine();
        ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(t.rows[i].c_str()); ImGui::PopTextWrapPos();
    }
    ImGui::PopFont();
}

void eventsView(App& a) {
    const auto& ev = a.rx.rpr.events;
    if (!ImGui::BeginTable("##rpre", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn(tr("Event"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& e : ev) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(e.utc).c_str());
        ImGui::TableNextColumn(); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(e.text.c_str()); ImGui::PopTextWrapPos();
    }
    ImGui::EndTable();
    if (ev.empty()) ImGui::TextDisabled("%s", tr("nothing heard yet"));
}

void tab(App& a) {
    presets(a);
    ImGui::Spacing();
    subNav("rprv", S.view, {"Signal", "Bursts", "Symbols", "Events"});
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    if (S.view == 0) signalView(a); else if (S.view == 1) burstsView(a); else if (S.view == 2) symbolsView(a); else eventsView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const auto& ev = a.rx.rpr.events;
    for (const auto& e : ev) { ImGui::PushFont(a.mono, 0); ImGui::TextDisabled("%s", dect2::timefmt::clock(e.utc).c_str()); ImGui::PopFont(); ImGui::SameLine(); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(e.text.c_str()); ImGui::PopTextWrapPos(); }
    if (ev.empty()) ImGui::TextDisabled("%s", tr("nothing heard yet"));
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::RprTelemetry& t = a.rx.rpr;
    auto kv = [&](const char* k, const char* fmt, auto... x) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, x...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv(tr("state"), "%s", dect2::rprSummary(t).c_str());
    kv(tr("analyses"), "%llu (%llu %s)", (unsigned long long)t.analyses, (unsigned long long)t.detections, tr("with a signal"));
    kv(tr("input"), "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kv(tr("signal time"), "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Signal"), !on ? 0 : a.rx.rpr.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp(tr("Bursts"), !on ? 0 : a.rx.rpr.dataValid ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::rprSummary(a.rx.rpr).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Robust Packet (RPR)";
    if (live(a)) l2 = dect2::rprSummary(a.rx.rpr);
}

} // namespace

extern const ModeUi kRprUi;
const ModeUi kRprUi = {.sideTitle = "EVENTS", .tabName = "Robust Packet", .tabIcon = Ic::Radio, .tab = tab, .receiver = receiver, .list = list, .status = status, .summary = summary};
