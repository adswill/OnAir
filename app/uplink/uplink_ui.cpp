// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode S uplink screens: counters of what is heard on 1030 MHz, then four views: the interrogators (II / SI code, interrogation rate,
// antenna rotation period from the main-beam bursts, level, last heard), the aircraft they address (how often, the Comm-B registers asked
// for, by whom; callsigns from ADS-B when that mode ran earlier in the session), the ACAS / TCAS activity, and the message log.
#include "app.h"
#include "dect2/adsb/adsb_tel.h"
#include "dect2/uplink/uplink_rx.h"
#include "dect2/uplink/uplink_tel.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>

namespace {

struct State {
    int view = 0;               // 0 interrogators, 1 aircraft, 2 TCAS, 3 log
    std::map<uint32_t, std::string> callsigns;   // from the ADS-B mode in this session
};
State S;

bool live(const App& a) { return a.rx.standard == 44; }   // the engine reports its standard code minus one

std::string ago(double now, double t) {
    char b[24];
    const double d = now - t;
    if (d < 1) snprintf(b, sizeof b, "now");
    else if (d < 120) snprintf(b, sizeof b, "%.0f s", d);
    else snprintf(b, sizeof b, "%.0f min", d / 60);
    return b;
}

std::string code(const dect2::UplinkInterrogator& i) {
    char b[24];
    if (i.kind == 1) return "Mode A/C";
    if (i.kind == 2) return "intermode";
    if (i.ii >= 0) snprintf(b, sizeof b, "II %d", i.ii); else snprintf(b, sizeof b, "SI %d", i.si);
    return b;
}

std::string name(uint32_t addr) {
    char b[40];
    auto it = S.callsigns.find(addr);
    if (it != S.callsigns.end()) snprintf(b, sizeof b, "%06X %s", addr, it->second.c_str()); else snprintf(b, sizeof b, "%06X", addr);
    return b;
}

void counters(App& a) {
    const dect2::UplinkTelemetry& t = a.rx.uplink;
    ImGui::PushFont(a.mono, 0);
    ImGui::Text("Mode A %llu  C %llu  sidelobe %llu   intermode A/C/S %llu  A/C %llu   Mode S %llu  P5 %llu", (unsigned long long)t.modeA, (unsigned long long)t.modeC,
                (unsigned long long)t.modeAcSidelobe, (unsigned long long)t.intermodeAcs, (unsigned long long)t.intermodeAc, (unsigned long long)t.modeS, (unsigned long long)t.modeSSidelobe);
    ImGui::Text("UF0 %llu  UF4 %llu  UF5 %llu  UF11 %llu  UF16 %llu  UF20 %llu  UF21 %llu  UF24 %llu", (unsigned long long)t.uf[0], (unsigned long long)t.uf[4], (unsigned long long)t.uf[5],
                (unsigned long long)t.uf[11], (unsigned long long)t.uf[16], (unsigned long long)t.uf[20], (unsigned long long)t.uf[21], (unsigned long long)t.uf[24]);
    ImGui::PopFont();
}

constexpr ImGuiTableFlags kTf = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV;

void interrogatorsView(App& a) {
    const dect2::UplinkTelemetry& t = a.rx.uplink;
    if (t.interrogators.empty()) { ImGui::TextDisabled("no interrogations yet"); return; }
    if (!ImGui::BeginTable("##upi", 8, kTf, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    for (const char* c : {"Code", "Heard", "Rate", "Rotation", "Level", "Last", "All-call / roll-call", "Mode A / C, sidelobe"}) ImGui::TableSetupColumn(c);
    ImGui::TableHeadersRow();
    for (const auto& i : t.interrogators) {
        ImGui::TableNextRow();
        ImGui::PushFont(a.mono, 0);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(code(i).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)i.count);
        ImGui::TableNextColumn(); ImGui::Text("%.1f /s", i.ratePerSec);
        ImGui::TableNextColumn(); if (i.periodSec > 0) ImGui::Text("%.2f s", i.periodSec); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::Text("%.1f dBFS", i.levelDb);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(t.timeSec, i.lastSec).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%llu / %llu", (unsigned long long)i.allCalls, (unsigned long long)i.rollCalls);
        ImGui::TableNextColumn(); ImGui::Text("%llu / %llu, %llu", (unsigned long long)i.modeA, (unsigned long long)i.modeC, (unsigned long long)i.sidelobe);
        ImGui::PopFont();
    }
    ImGui::EndTable();
}

void aircraftView(App& a) {
    const dect2::UplinkTelemetry& t = a.rx.uplink;
    if (t.aircraft.empty()) { ImGui::TextDisabled("no aircraft addressed yet"); return; }
    if (!ImGui::BeginTable("##upa", 6, kTf, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    for (const char* c : {"Address", "Interrogations", "Formats", "Registers", "By", "Last"}) ImGui::TableSetupColumn(c);
    ImGui::TableHeadersRow();
    for (const auto& x : t.aircraft) {
        ImGui::TableNextRow();
        ImGui::PushFont(a.mono, 0);
        ImGui::TableNextColumn();
        if (x.confirmed) ImGui::TextUnformatted(name(x.addr).c_str()); else ImGui::TextDisabled("%s ?", name(x.addr).c_str());
        if (!x.confirmed && ImGui::IsItemHovered()) ImGui::SetTooltip("Heard once: a bit error gives a wrong address, so it waits for a second interrogation.");
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)x.count);
        ImGui::TableNextColumn();
        std::string f;
        for (int u : {0, 4, 5, 16, 20, 21, 24}) if (x.uf[u]) { char b[24]; snprintf(b, sizeof b, "%sUF%d:%llu", f.empty() ? "" : " ", u, (unsigned long long)x.uf[u]); f += b; }
        ImGui::TextUnformatted(f.c_str());
        ImGui::TableNextColumn();
        std::string r;
        for (const auto& s : x.registers) r += (r.empty() ? "" : " ") + s;
        ImGui::TextUnformatted(r.empty() ? "-" : r.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(x.interrogators.empty() ? "-" : x.interrogators.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(t.timeSec, x.lastSec).c_str());
        ImGui::PopFont();
    }
    ImGui::EndTable();
}

void tcasView(App& a) {
    const dect2::UplinkTelemetry& t = a.rx.uplink;
    if (t.tcas.empty()) { ImGui::TextDisabled("no ACAS interrogations (UF0 / UF16) yet"); return; }
    if (!ImGui::BeginTable("##upt", 3, kTf, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Format"); ImGui::TableSetupColumn("What", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (auto it = t.tcas.rbegin(); it != t.tcas.rend(); ++it) {
        ImGui::TableNextRow();
        ImGui::PushFont(a.mono, 0);
        ImGui::TableNextColumn(); ImGui::Text("%.1f s", it->sec);
        ImGui::TableNextColumn(); ImGui::Text("UF%d", it->uf);
        ImGui::TableNextColumn();
        if (it->uf == 16 && it->from) ImGui::TextColored(ImVec4(0.98f, 0.72f, 0.36f, 1), "%s  (%s -> %s)", it->text.c_str(), name(it->from).c_str(), name(it->to).c_str());
        else ImGui::TextUnformatted(it->text.c_str());
        ImGui::PopFont();
    }
    ImGui::EndTable();
}

void logView(App& a) {
    const dect2::UplinkTelemetry& t = a.rx.uplink;
    if (!ImGui::BeginTable("##upl", 4, kTf, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("Address"); ImGui::TableSetupColumn("Interrogation", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (auto it = t.log.rbegin(); it != t.log.rend(); ++it) {
        ImGui::TableNextRow();
        ImGui::PushFont(a.mono, 0);
        ImGui::TableNextColumn(); ImGui::Text("%.4f", it->sec);
        ImGui::TableNextColumn(); ImGui::Text("%.0f", it->levelDb);
        ImGui::TableNextColumn(); if (it->uf >= 0 && it->uf != 11) ImGui::TextUnformatted(name(it->addr).c_str()); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(it->text.c_str());
        ImGui::PopFont();
    }
    ImGui::EndTable();
}

void tab(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "Start the receiver on 1030 MHz (8 Msps or more)"); return; }
    counters(a);
    ImGui::Spacing();
    subNav("upv", S.view, {"Interrogators", "Aircraft", "TCAS", "Log"});
    ImGui::Spacing();
    if (S.view == 0) interrogatorsView(a);
    else if (S.view == 1) aircraftView(a);
    else if (S.view == 2) tcasView(a);
    else logView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    if (resultsClearButton("uplink")) a.engine.uplink().clearResults();
    const dect2::UplinkTelemetry& t = a.rx.uplink;
    if (!ImGui::BeginTable("##upr", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Interrogator"); ImGui::TableSetupColumn("Rotation", ImGuiTableColumnFlags_WidthStretch);
    for (const auto& i : t.interrogators) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(code(i).c_str()); ImGui::PopFont();
        ImGui::TableNextColumn();
        if (i.periodSec > 0) ImGui::TextDisabled("%.2f s, %.1f /s", i.periodSec, i.ratePerSec); else ImGui::TextDisabled("%.1f /s", i.ratePerSec);
    }
    ImGui::EndTable();
    if (t.interrogators.empty()) ImGui::TextDisabled("no interrogators yet");
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::UplinkTelemetry& t = a.rx.uplink;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("state", "%s", dect2::uplinkSummary(t).c_str());
    kv("input", "%.3f Msps%s, noise %.1f dBFS", t.inputRate / 1e6, t.inputRate < 8e6 - 1 ? " (too low: 8 Msps or more)" : "", t.noiseDb);
    kv("pulses", "%llu P1 candidates", (unsigned long long)t.pulses);
    kv("decoded", "%llu interrogations, %llu Mode S without sync or with bad parity", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kv("offset", "%.1f kHz (Mode S mean)", t.cfoHz / 1e3);
    kv("newest", "%.1f dB over the noise", t.snrDb);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Pulses", !on ? 0 : a.rx.uplink.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("Decoded", !on ? 0 : a.rx.uplink.state == 2 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::uplinkSummary(a.rx.uplink).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Mode S uplink";
    if (live(a)) l2 = dect2::uplinkSummary(a.rx.uplink);
}

} // namespace

// app_state.cpp, with every ADS-B report: the callsigns, so that the uplink tables can show them
void uplinkNoteAdsb(const dect2::AdsbTelemetry& t) {
    for (const auto& ac : t.aircraft)
        if (!ac.callsign.empty()) S.callsigns[ac.icao & 0xFFFFFF] = ac.callsign;
    if (S.callsigns.size() > 5000) S.callsigns.clear();
}

extern const ModeUi kUplinkUi;
const ModeUi kUplinkUi = {
    .sideTitle = "INTERROGATORS",
    .tabName = "Uplink",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
};
