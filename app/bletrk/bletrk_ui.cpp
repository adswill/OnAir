// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// BLE tracker detector screens: the trackers heard (kind, state, how long and how close each stays, level trend), the alerts for a tracker that
// follows you, and the settings of the alert. Receive only: trackers are recognised from their public advertisements; nothing is sent to them.
// Identifiers are kept in memory for a short window and shown as the last two address bytes.
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "dect2/ble/ble_proto.h"
#include "dect2/bletrk/bletrk_live.h"
#include "dect2/bletrk/bletrk_proto.h"
#include "dect2/bletrk/bletrk_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;                 // 0 trackers, 1 alerts
    bool loaded = false;
    int alertMin = 10;            // minutes a tracker must stay
    int nearDb = -60;             // averaged level counted as near
    int pushedMin = -1, pushedNear = 1;
    uint32_t sel = 0;
};
State S;

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.alertMin = std::max(1, std::min(120, (int)plat::prefs().getI("bletrkMin", 10)));
    S.nearDb = std::max(-100, std::min(0, (int)plat::prefs().getI("bletrkNear", -60)));
}

bool live(const App& a) { return a.rx.standard == 144; }   // the engine reports its standard code minus one

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

std::string clock(int64_t t) {
    if (t <= 0) return "-";
    return dect2::timefmt::clock(t);
}

std::string span(double sec) {
    char b[24];
    if (sec < 90) snprintf(b, sizeof b, "%.0f s", sec);
    else if (sec < 5400) snprintf(b, sizeof b, "%.0f min", sec / 60);
    else snprintf(b, sizeof b, "%.1f h", sec / 3600);
    return b;
}

void pushSettings(App& a) {
    if (S.alertMin != S.pushedMin) { a.engine.bletrk().setAlertSeconds(S.alertMin * 60.0); S.pushedMin = S.alertMin; }
    if (S.nearDb != S.pushedNear) { a.engine.bletrk().setNearDbfs((float)S.nearDb); S.pushedNear = S.nearDb; }
}

void tick(App& a) { loadState(); pushSettings(a); }

void notice() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Receive only: item trackers are recognised from their public advertisements. OnAir never connects to one, sends to one or reads its owner's data. "
                                 "Identifiers stay in memory for 30 minutes after a tracker was last heard, are shown as the last two address bytes only and are never saved."));
    ImGui::PopTextWrapPos();
}

const char* stateText(const dect2::BletrkTracker& d) {
    if (d.ignored) return tr("your own");
    if (d.following) return tr("following you");
    if (d.separated) return tr("separated from its owner");
    return tr("nearby");
}

void trackersView(App& a) {
    notice();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to see the trackers around you")); return; }
    const dect2::BletrkTelemetry& t = a.rx.bletrk;
    if (ImGui::Button(tr("Clear"))) { a.engine.bletrk().clearResults(); S.sel = 0; }
    ImGui::SameLine(0, 10 * gUi);
    ImGui::TextDisabled("%s", dect2::bletrkSummary(t).c_str());
    if (!ImGui::BeginTable("##btt", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Tracker")); ImGui::TableSetupColumn(tr("State")); ImGui::TableSetupColumn(tr("Stays")); ImGui::TableSetupColumn(tr("Near"));
    ImGui::TableSetupColumn(tr("Level")); ImGui::TableSetupColumn(tr("Trend")); ImGui::TableSetupColumn(tr("Addresses")); ImGui::TableSetupColumn(tr("Last seen"));
    ImGui::TableSetupColumn("##mine", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& d : t.trackers) {
        ImGui::TableNextRow();
        ImGui::PushID((int)d.id);
        const double ago = std::max(0.0, t.timeSec - d.lastSec);
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, d.following ? pal::badRed() : ago < 5 ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text));
        if (ImGui::Selectable(d.label.c_str(), S.sel == d.id, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.sel = d.id;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n%llu packets, first heard %s%s, peak %.0f dBFS", dect2::bletrk::kindName(d.kind), (unsigned long long)d.packets, clock(d.firstWall).c_str(), dect2::timefmt::sfx(), (double)d.rssiPeak);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(stateText(d));
        ImGui::TableNextColumn(); ImGui::TextUnformatted(span(d.lastSec - d.firstSec).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.0f%%", (double)(d.nearFrac * 100));
        ImGui::TableNextColumn(); ImGui::Text("%.0f", (double)d.rssiAvg);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(d.trend > 0 ? tr("closer") : d.trend < 0 ? tr("farther") : tr("steady"));
        ImGui::TableNextColumn(); ImGui::Text("%d", d.addresses);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(span(ago).c_str());
        ImGui::TableNextColumn();
        if (ImGui::SmallButton(d.ignored ? tr("Unmark") : tr("Mark as mine"))) a.engine.bletrk().ignoreTracker(d.id, !d.ignored);
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (t.trackers.empty()) ImGui::TextDisabled("%s", tr("no trackers heard yet"));
}

void alertsView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::BletrkTelemetry& t = a.rx.bletrk;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("An alert is also sent to the watchlists: add the word tracker to a list of the Words kind to get a notification or a push message."));
    ImGui::PopTextWrapPos();
    if (!ImGui::BeginTable("##bta", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Time")); ImGui::TableSetupColumn(tr("Level")); ImGui::TableSetupColumn(tr("Alert"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& al : t.alerts) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(al.wallTime).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.0f", (double)al.rssiAvg);
        ImGui::TableNextColumn(); ImGui::TextColored(pal::badRed(), "%s", al.text.c_str());
    }
    ImGui::EndTable();
    if (t.alerts.empty()) ImGui::TextDisabled("%s", tr("no alerts yet"));
}

void tab(App& a) {
    subNav("btv", S.view, {tr("Trackers"), tr("Alerts")});
    if (S.view == 0) trackersView(a); else alertsView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::BletrkTelemetry& t = a.rx.bletrk;
    if (!ImGui::BeginTable("##btl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn(tr("Tracker"), ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn(tr("Stays"));
    for (const auto& d : t.trackers) {
        ImGui::TableNextRow();
        ImGui::PushID((int)d.id);
        ImGui::TableNextColumn();
        if (ImGui::Selectable("##row", S.sel == d.id, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.sel = d.id;
        ImGui::SameLine(0, 0);
        ImGui::TextColored(d.following ? pal::badRed() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", d.label.c_str());
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", span(d.lastSec - d.firstSec).c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::BletrkTelemetry& t = a.rx.bletrk;
    kvRow(a, tr("state"), "%s", dect2::bletrkSummary(t).c_str());
    if (t.channel) kvRow(a, tr("channel"), "%d (%.0f MHz)", t.channel, dect2::ble::channelMhz(t.channel));
    kvRow(a, tr("signal"), "eye SNR %.1f dB, offset %+.1f kHz", (double)t.snrDb, t.cfoHz / 1e3);
    kvRow(a, tr("packets"), "%llu good, %llu failed CRC, %llu from trackers", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.trackerPackets);
    std::string now, win;
    for (int k = 1; k < dect2::bletrk::kKinds; k++) {
        char b[64];
        snprintf(b, sizeof b, "%s%s %d", now.empty() ? "" : ", ", dect2::bletrk::kindShort(k), t.nowByKind[k]); now += b;
        snprintf(b, sizeof b, "%s%s %d", win.empty() ? "" : ", ", dect2::bletrk::kindShort(k), t.windowByKind[k]); win += b;
    }
    kvRow(a, tr("in the last minute"), "%s", now.c_str());
    kvRow(a, tr("in the last 30 min"), "%s", win.c_str());
    kvRow(a, tr("alert after"), "%.0f min, near above %.0f dBFS", t.alertSec / 60, (double)t.nearDbfs);
    kvRow(a, tr("input"), "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, (double)t.levelDb);
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("A tracker that stays near a fixed receiver, such as a neighbour's tag, is reported too, and the levels are the radio's own, not calibrated distances. "
                                 "The detector is meant for a receiver that travels with you."));
    ImGui::PopTextWrapPos();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Adverts"), !on ? 0 : a.rx.bletrk.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp(tr("Alert"), !on ? 0 : a.rx.bletrk.followingNow > 0 ? 3 : 1); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::bletrkSummary(a.rx.bletrk).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "BLE tracker detector";
    if (live(a)) l2 = dect2::bletrkSummary(a.rx.bletrk);
}

void tuner(App& a, bool& retune) {
    loadState();
    ImGui::TextDisabled("%s", tr("Channel"));
    const int cur = dect2::ble::advChannelOf(a.freqMhz);
    char label[32];
    if (cur) snprintf(label, sizeof label, "%d (%.0f MHz)", cur, dect2::ble::channelMhz(cur)); else snprintf(label, sizeof label, "%s", tr("other"));
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(130 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##btch", label)) {
        for (int c = 37; c <= 39; c++) {
            char b[40];
            snprintf(b, sizeof b, "%d  %.0f MHz", c, dect2::ble::channelMhz(c));
            if (ImGui::Selectable(b, c == cur)) { tuneFreq(a, dect2::ble::channelMhz(c)); retune = true; }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Trackers advertise on all three channels in turn, so any one of them hears every tracker."));
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Alert after")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(90 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::SliderInt("##btmin", &S.alertMin, 1, 60, "%d min")) { plat::prefs().setI("bletrkMin", S.alertMin); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("How long a tracker must stay with the receiver before the alert."));
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Near level")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(100 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::SliderInt("##btnear", &S.nearDb, -100, -20, "%d dBFS")) { plat::prefs().setI("bletrkNear", S.nearDb); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The averaged level from which a tracker counts as near. The radio's level, not a calibrated distance."));
}

} // namespace

extern const ModeUi kBletrkUi;
const ModeUi kBletrkUi = {
    .sideTitle = "TRACKERS",
    .tabName = "Trackers",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .tick = tick,
};
