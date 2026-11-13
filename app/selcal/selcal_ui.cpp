// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SELCAL screens (ARINC 714, the tone call that alerts an aircraft's crew): the call log (time, code, the four tone frequencies, dial frequency,
// aircraft from your own table), the codes heard (one row per code) and the audio spectrum. A code names an airframe, it carries no position,
// so there is no map: a file selcal_aircraft.tsv in the data folder (CODE<TAB>registration) shows the registration beside each call.
#include "app.h"
#include "i18n/i18n.h"
#include "dect2/common/time_fmt.h"
#include "dect2/selcal/selcal_rx.h"
#include "dect2/selcal/selcal_tel.h"
#include "dect2/tones/tones_rx.h"
#include "platform/platform.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

struct State {
    int view = 0;
    bool listen = false, loaded = false, tableSent = false;
    int demod = -1;                 // -1 auto (USB below 30 MHz, AM above), else dect2::TonesDemod
    int pushedDemod = -2;
    bool wasRunning = false;
};
State S;

bool live(const App& a) { return a.rx.standard == 241; }   // the engine reports its standard code minus one

void load() {
    if (S.loaded) return;
    S.loaded = true;
    S.listen = plat::prefs().getB("selcalListen", false);
    S.demod = (int)std::max(-1L, std::min((long)dect2::TonesDemodCount - 1, (long)plat::prefs().getI("selcalDemod", -1)));
}

int resolvedDemod(const App& a) { return S.demod >= 0 ? S.demod : (a.freqMhz < 30.0 ? (int)dect2::TonesUsb : (int)dect2::TonesAm); }

void banner(App& a) {
    load();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Tune an aeronautical voice channel (HF in USB, VHF in AM). Each SELCAL call is logged."));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("SELCAL (ARINC 714) is a call of two pairs of tones, 1 s each, from 16 tones (letters A B C D E F G H J K L M P Q R S, 312.6 - 1479.1 Hz). A code belongs to one aircraft: it carries no position, so no map. Put a file selcal_aircraft.tsv (CODE, tab, registration) in the data folder to see the aircraft."));
    flowNext(8 * gUi);
    listenControls(a, S.listen, "selcalListen", "selcal", tr("Play the channel audio; decoding is the same either way"));
    flowNext(8 * gUi);
    static const char* names[] = {"Auto", "FM", "USB", "LSB", "AM"};
    ImGui::SetNextItemWidth(80 * gUi);
    if (ImGui::BeginCombo("##selcaldemod", names[S.demod + 1])) {
        for (int i = -1; i < dect2::TonesDemodCount; i++) if (ImGui::Selectable(names[i + 1], S.demod == i)) { S.demod = i; plat::prefs().setI("selcalDemod", i); plat::prefs().flush(); }
        ImGui::EndCombo();
    }
    flowNext(8 * gUi);
    char cl[40]; snprintf(cl, sizeof cl, "%s##selcal", tr("Clear"));
    if (ImGui::Button(cl)) a.engine.selcal().clearResults();
    flowEnd();
}

std::string khz(double hz) { char b[32]; if (hz <= 0) return "-"; snprintf(b, sizeof b, "%.1f", hz / 1e3); return b; }

void callsView(App& a) {
    const auto& cs = a.rx.selcal.calls;
    if (!ImGui::BeginTable("##selcalc", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn(tr("Code")); ImGui::TableSetupColumn(tr("Tones (Hz)")); ImGui::TableSetupColumn(tr("Frequency (kHz)"));
    ImGui::TableSetupColumn("dB"); ImGui::TableSetupColumn(tr("Aircraft"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    for (const auto& c : cs) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(c.utc).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%s", c.code.c_str());
        ImGui::TableNextColumn(); ImGui::TextDisabled("%.1f %.1f / %.1f %.1f", c.hz[0], c.hz[1], c.hz[2], c.hz[3]);
        ImGui::TableNextColumn(); ImGui::Text("%s", khz(c.dialHz).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.0f", c.levelDb);
        ImGui::TableNextColumn(); ImGui::Text("%s", c.aircraft.c_str());
    }
    ImGui::PopFont();
    ImGui::EndTable();
    if (cs.empty()) ImGui::TextDisabled("%s", tr("no calls yet"));
}

void codesView(App& a) {
    const auto& cd = a.rx.selcal.codes;
    if (!ImGui::BeginTable("##selcalk", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Code")); ImGui::TableSetupColumn(tr("Aircraft")); ImGui::TableSetupColumn(tr("Last heard")); ImGui::TableSetupColumn(tr("Heard"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    for (const auto& c : cd) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%s", c.code.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%s", c.aircraft.c_str());
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(c.lastSeen).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%llux", (unsigned long long)c.count);
    }
    ImGui::PopFont();
    ImGui::EndTable();
    if (cd.empty()) ImGui::TextDisabled("%s", tr("no codes yet"));
}

void tab(App& a) {
    banner(a);
    ImGui::Spacing();
    subNav("selcalv", S.view, {"Calls", "Codes"});
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    if (S.view == 0) callsView(a); else codesView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const auto& cd = a.rx.selcal.codes;
    if (!ImGui::BeginTable("##selcall", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn(tr("Code")); ImGui::TableSetupColumn(tr("Heard"), ImGuiTableColumnFlags_WidthStretch);
    for (const auto& c : cd) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::Text("%s", c.code.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s %s, %llux", c.aircraft.c_str(), dect2::timefmt::clock(c.lastSeen).c_str(), (unsigned long long)c.count);
    }
    ImGui::EndTable();
    if (cd.empty()) ImGui::TextDisabled("%s", tr("no codes yet"));
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::SelcalTelemetry& t = a.rx.selcal;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv(tr("state"), "%s", dect2::selcalSummary(t).c_str());
    kv(tr("calls"), "%llu", (unsigned long long)t.blocksOk);
    kv(tr("channel"), "%s", dect2::tonesDemodName(t.demod));
    kv(tr("audio"), "%.1f dB", t.audioDb);
    if (t.toneHz > 0) kv(tr("strongest tone"), "%.1f Hz, %.0f dB", t.toneHz, t.toneDb);
    kv(tr("input"), "%.3f Msps", t.inputRate / 1e6);
    kv(tr("signal time"), "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Audio"), !on ? 0 : a.rx.selcal.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("SELCAL", !on ? 0 : a.rx.selcal.dataValid ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::selcalSummary(a.rx.selcal).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "SELCAL";
    if (live(a)) l2 = dect2::selcalSummary(a.rx.selcal);
}

void tick(App& a) {
    load();
    dect2::SelcalReceiver& r = a.engine.selcal();
    if (!S.tableSent) {
        S.tableSent = true;
        std::ifstream in(std::filesystem::u8path(plat::dataDir() + "/selcal_aircraft.tsv"), std::ios::binary);
        if (in) { std::stringstream ss; ss << in.rdbuf(); r.setAircraftTable(ss.str()); }
    }
    const int d = resolvedDemod(a);
    if (d != S.pushedDemod) { S.pushedDemod = d; r.setDemod(d); }
    r.setListen(S.listen); r.setVolume(a.volume); r.setMuted(a.muted);
}

} // namespace

extern const ModeUi kSelcalUi;
const ModeUi kSelcalUi = {
    .sideTitle = "CODES",
    .tabName = "SELCAL",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
