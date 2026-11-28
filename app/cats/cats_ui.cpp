// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CATS (balloon packet radio, #643) screens (receive only): the decoded packets (time, callsign and SSID, position, altitude, comment) and the events.
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "gfx/gfx.h"
#include "i18n/i18n.h"
#include "dect2/cats/cats_rx.h"
#include "dect2/cats/cats_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct State { int view = 0; } S;

bool live(const App& a) { return a.rx.standard == 901; }   // the engine reports its standard code minus one

void presets(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("CATS: 9600 bd 2-FSK, the frequency is set in the tuning bar"));
    flowNext(8 * gUi);
    char cl[40]; snprintf(cl, sizeof cl, "%s##catsclr", tr("Clear"));
    if (ImGui::Button(cl)) a.engine.cats().clearResults();
    flowEnd();
}

void packetsView(App& a) {
    const auto& pk = a.rx.cats.packets;
    if (!ImGui::BeginTable("##catsp", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn(tr("Callsign")); ImGui::TableSetupColumn(tr("Position")); ImGui::TableSetupColumn(tr("Altitude"));
    ImGui::TableSetupColumn(tr("Comment"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& p : pk) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(p.utc).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%s-%d", p.callsign.c_str(), p.ssid);
        ImGui::TableNextColumn(); if (p.hasGps) ImGui::Text("%.5f, %.5f", p.lat, p.lon); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (p.hasGps) ImGui::Text("%.0f m", p.altM); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(p.comment.c_str()); ImGui::PopTextWrapPos();
    }
    ImGui::EndTable();
    if (pk.empty()) ImGui::TextDisabled("%s", tr("no packet yet"));
}

void eventsView(App& a) {
    const auto& ev = a.rx.cats.events;
    if (!ImGui::BeginTable("##catse", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
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
    subNav("catsv", S.view, {"Packets", "Events"});
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    if (S.view == 0) packetsView(a); else eventsView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const auto& ev = a.rx.cats.events;
    for (const auto& e : ev) { ImGui::PushFont(a.mono, 0); ImGui::TextDisabled("%s", dect2::timefmt::clock(e.utc).c_str()); ImGui::PopFont(); ImGui::SameLine(); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(e.text.c_str()); ImGui::PopTextWrapPos(); }
    if (ev.empty()) ImGui::TextDisabled("%s", tr("nothing heard yet"));
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::CatsTelemetry& t = a.rx.cats;
    auto kv = [&](const char* k, const char* fmt, auto... x) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, x...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv(tr("state"), "%s", dect2::catsSummary(t).c_str());
    kv(tr("input"), "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kv(tr("signal time"), "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Packets"), !on ? 0 : a.rx.cats.dataValid ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::catsSummary(a.rx.cats).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "CATS";
    if (live(a)) l2 = dect2::catsSummary(a.rx.cats);
}

} // namespace

extern const ModeUi kCatsUi;
const ModeUi kCatsUi = {.sideTitle = "EVENTS", .tabName = "CATS", .tabIcon = Ic::Radio, .tab = tab, .receiver = receiver, .list = list, .status = status, .summary = summary};
