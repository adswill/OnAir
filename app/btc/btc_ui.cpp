// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Bluetooth Classic screens: the devices and piconets heard (LAP, and for a discoverable device its full address and class of device), the access
// code log, the receiver details. Receive only: it never inquires, pages or answers; it listens to what devices and piconets send anyway.
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "dect2/btc/btc_proto.h"
#include "dect2/btc/btc_rx.h"
#include "dect2/btc/btc_tel.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;                        // 0 devices, 1 access codes
    char filter[64] = "";
    float minRssi = -120;
    bool respondersOnly = false;
    uint32_t sel = 0;                    // LAP clicked in the table: the log shows only its packets
    bool hasSel = false;
};
State S;

bool live(const App& a) { return a.rx.standard == 381; }   // the engine reports its standard code minus one

std::string clock(int64_t t) { return t <= 0 ? std::string("-") : dect2::timefmt::clock(t); }

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

bool passes(const dect2::BtcDevice& d) {
    if (d.rssi < S.minRssi) return false;
    if (S.respondersOnly && !d.hasFhs) return false;
    if (!S.filter[0]) return true;
    const std::string f = lower(S.filter);
    return lower(d.lapText).find(f) != std::string::npos || lower(d.addr).find(f) != std::string::npos || lower(d.cod).find(f) != std::string::npos ||
           lower(d.kind).find(f) != std::string::npos;
}

void privacyNote() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Receive only: OnAir listens and never inquires, pages or answers. Every Bluetooth Classic packet starts with the sync word of the "
                        "master's LAP, so a piconet in use shows up by its LAP. A device that is discoverable and answers an inquiry also gives its full "
                        "address and class of device. The radio hops over 79 channels, so a capture of 20 MHz sees about a quarter of the packets. "
                        "Names are not sent in these packets. Bluetooth LE devices are in the Bluetooth LE mode.");
    ImGui::PopTextWrapPos();
}

int channelsHeard(const dect2::BtcDevice& d) { return __builtin_popcountll(d.chanLo) + __builtin_popcountll(d.chanHi); }

void devicesView(App& a) {
    privacyNote();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the devices around you"); return; }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Filter"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(160 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##btcf", "LAP, address, class", S.filter, sizeof S.filter);
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Level above"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(120 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::SliderFloat("##btcr", &S.minRssi, -120, 0, "%.0f dBFS");
    flowNext(10 * gUi);
    ImGui::Checkbox("discoverable only", &S.respondersOnly);
    flowNext(10 * gUi);
    if (ImGui::Button("Clear")) a.engine.btc().clearDevices();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forget every device and packet heard so far");
    flowBreak();
    const dect2::BtcTelemetry& t = a.rx.btc;
    if (!ImGui::BeginTable("##btcdev", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("LAP"); ImGui::TableSetupColumn("Address"); ImGui::TableSetupColumn("Kind"); ImGui::TableSetupColumn("Level");
    ImGui::TableSetupColumn("Last seen"); ImGui::TableSetupColumn("Packets"); ImGui::TableSetupColumn("Channels"); ImGui::TableSetupColumn("Class of device", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    int shown = 0;
    for (const auto& d : t.devices) {
        if (!passes(d)) continue;
        shown++;
        ImGui::TableNextRow();
        ImGui::PushID((int)d.lap);
        const double ago = std::max(0.0, t.timeSec - d.lastSec);
        const ImVec4 c = ago < 3 ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::TableNextColumn();
        const bool isSel = S.hasSel && S.sel == d.lap;
        if (ImGui::Selectable("##row", isSel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) { S.hasSel = !isSel; S.sel = d.lap; }
        ImGui::SameLine(0, 0);
        ImGui::PushFont(a.mono, 0); ImGui::TextColored(c, "%s", d.lapText.c_str()); ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("first heard %s%s\nlast on %.0f MHz", clock(d.firstWall).c_str(), dect2::timefmt::sfx(), d.lastMhz);
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(d.addr.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(d.kind.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%4.0f", d.rssi);
        if (d.rssiHist.size() >= 2) {
            ImGui::SameLine(0, 4 * gUi);
            ImGui::PlotLines("##h", d.rssiHist.data(), (int)d.rssiHist.size(), 0, nullptr, -110, -10, ImVec2(48 * gUi, ImGui::GetTextLineHeight()));
        }
        ImGui::TableNextColumn(); ImGui::Text(ago < 1 ? "now" : "%.0f s ago", ago);
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)d.packets);
        ImGui::TableNextColumn(); ImGui::Text("%d", channelsHeard(d));
        ImGui::TableNextColumn(); ImGui::TextUnformatted(d.cod.c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (t.devices.empty()) ImGui::TextDisabled("nothing heard yet");
    else if (!shown) ImGui::TextDisabled("no device passes the filter");
}

void packetsView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the access codes"); return; }
    const dect2::BtcTelemetry& t = a.rx.btc;
    if (S.hasSel) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(pal::okGreen(), "Showing LAP %s only", dect2::btc::lapText(S.sel).c_str());
        ImGui::SameLine(0, 8 * gUi);
        if (ImGui::SmallButton("Show all")) S.hasSel = false;
    }
    std::vector<size_t> rows;
    for (size_t i = 0; i < t.log.size(); i++) if (!S.hasSel || t.log[i].lap == S.sel) rows.push_back(i);
    if (ImGui::Button("Copy")) {
        std::string txt;
        for (size_t i : rows) {
            const auto& p = t.log[i];
            char b[600];
            snprintf(b, sizeof b, "%s\t%.0f\t%s\t%.0f\t%s\n", clock(p.wallTime).c_str(), p.mhz, dect2::btc::lapText(p.lap).c_str(), p.rssi, p.info.c_str());
            txt += b;
        }
        ImGui::SetClipboardText(txt.c_str());
    }
    if (!ImGui::BeginTable("##btclog", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(dect2::timefmt::headTime()); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("LAP"); ImGui::TableSetupColumn("Level");
    ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGuiListClipper clip;
    clip.Begin((int)rows.size());
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const auto& p = t.log[rows[(size_t)i]];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(p.wallTime).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.0f", p.mhz);
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(dect2::btc::lapText(p.lap).c_str()); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::Text("%.0f", p.rssi);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.info.c_str());
        }
    ImGui::EndTable();
    if (t.log.empty()) ImGui::TextDisabled("no access codes yet");
    else if (rows.empty()) ImGui::TextDisabled("no packet passes the filter");
}

// ---------------------------------------------------------------- hooks

void tab(App& a) {
    subNav("btcv", S.view, {"Devices", "Access codes"});
    if (S.view == 0) devicesView(a); else packetsView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::BtcTelemetry& t = a.rx.btc;
    if (!ImGui::BeginTable("##btcl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Level");
    for (const auto& d : t.devices) {
        if (!passes(d)) continue;
        ImGui::TableNextRow();
        ImGui::PushID((int)d.lap);
        const bool recent = t.timeSec - d.lastSec < 3;
        ImGui::TableNextColumn();
        const bool isSel = S.hasSel && S.sel == d.lap;
        if (ImGui::Selectable("##row", isSel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) { S.hasSel = !isSel; S.sel = d.lap; }
        ImGui::SameLine(0, 0);
        ImGui::PushFont(a.mono, 0); ImGui::TextColored(recent ? pal::okGreen() : pal::grey(0.6f), "%s", d.hasFhs ? d.addr.c_str() : d.lapText.c_str()); ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", d.kind.c_str(), d.cod.c_str());
        ImGui::TableNextColumn(); ImGui::TextDisabled("%.0f", d.rssi);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::BtcTelemetry& t = a.rx.btc;
    kvRow(a, "state", "%s", dect2::btcSummary(t).c_str());
    kvRow(a, "coverage", "%d channels of 1 MHz%s", t.channelsCovered, t.centerMhz > 0 ? "" : " (centre taken as 2441 MHz)");
    kvRow(a, "signal", "SNR %.1f dB, offset %+.1f kHz", t.snrDb, t.cfoHz / 1e3);
    kvRow(a, "access codes", "%llu found, %llu bursts without one", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kvRow(a, "inquiry", "%llu ID packets (something is scanning), %llu responses decoded", (unsigned long long)t.giacIds, (unsigned long long)t.fhsCount);
    kvRow(a, "bursts", "%llu analysed, %llu wideband skipped (Wi-Fi and the like)", (unsigned long long)t.burstsSeen, (unsigned long long)t.burstsWide);
    kvRow(a, "input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kvRow(a, "signal time", "%.1f s", t.timeSec);
    ImGui::Spacing();
    privacyNote();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Access codes", !on ? 0 : a.rx.btc.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::btcSummary(a.rx.btc).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Bluetooth Classic";
    if (live(a)) l2 = dect2::btcSummary(a.rx.btc);
}

} // namespace

extern const ModeUi kBtcUi;
const ModeUi kBtcUi = {
    .sideTitle = "DEVICES",
    .tabName = "Devices",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
};
