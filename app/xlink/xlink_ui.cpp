// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Extra links screens: the links heard and their frames. Receive only; encrypted payloads stay structure only.
#include "app.h"
#include "i18n/i18n.h"
#include "dect2/common/time_fmt.h"
#include "dect2/xlink/xlink_proto.h"
#include "dect2/xlink/xlink_rx.h"
#include "dect2/xlink/xlink_tel.h"
#include <cstdio>
#include <string>

namespace {

struct State { int view = 0; };
State S;

bool live(const App& a) { return a.rx.standard == 601 || a.rx.standard == 602; }   // the engine reports its standard code minus one

void framesView(App& a) {
    const auto& fr = a.rx.xlink.frames;
    if (!ImGui::BeginTable("##xlf", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn(tr("Link")); ImGui::TableSetupColumn(tr("Who")); ImGui::TableSetupColumn(tr("Type"));
    ImGui::TableSetupColumn(tr("Content"), ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn(tr("MHz")); ImGui::TableSetupColumn("SNR");
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    for (const auto& f : fr) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(f.utc).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%s", dect2::xlink::protoName(f.proto));
        ImGui::TableNextColumn(); ImGui::Text("%s", f.id.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%s", f.type.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(f.detail.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.3f", f.freqHz / 1e6);
        ImGui::TableNextColumn(); ImGui::Text("%.0f", f.snrDb);
    }
    ImGui::PopFont();
    ImGui::EndTable();
}

void linksView(App& a) {
    const auto& dv = a.rx.xlink.links;
    if (dv.empty()) { ImGui::TextDisabled("%s", tr("no links heard yet")); return; }
    if (!ImGui::BeginTable("##xld", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Link")); ImGui::TableSetupColumn(tr("Who")); ImGui::TableSetupColumn(tr("Last"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(tr("Lost")); ImGui::TableSetupColumn(tr("Heard"));
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    for (const auto& d : dv) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%s", dect2::xlink::protoName(d.proto));
        ImGui::TableNextColumn(); ImGui::Text("%s", d.id.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%s: %s", d.lastType.c_str(), d.lastDetail.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)d.lost);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s, %llux", dect2::timefmt::clock(d.lastSeen).c_str(), (unsigned long long)d.count);
    }
    ImGui::PopFont();
    ImGui::EndTable();
}

void tab(App& a) {
    char cl[40]; snprintf(cl, sizeof cl, "%s##xl", tr("Clear"));
    ImGui::AlignTextToFramePadding();
    if (ImGui::Button(cl)) a.engine.xlink().clearResults();
    ImGui::Spacing();
    subNav("xlv", S.view, {"Links", "Frames"});
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    if (S.view == 0) linksView(a); else framesView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const auto& dv = a.rx.xlink.links;
    if (!ImGui::BeginTable("##xll", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn(tr("Who")); ImGui::TableSetupColumn(tr("Heard"), ImGuiTableColumnFlags_WidthStretch);
    for (const auto& d : dv) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::Text("%s", d.id.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s, %llux", dect2::timefmt::clock(d.lastSeen).c_str(), (unsigned long long)d.count);
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::XlinkTelemetry& t = a.rx.xlink;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv(tr("state"), "%s", dect2::xlinkSummary(t).c_str());
    kv(tr("frames"), "%llu", (unsigned long long)t.blocksOk);
    kv(tr("802.11n frames"), "%llu", (unsigned long long)t.phyFrames);
    kv(tr("not decodable here"), "%llu", (unsigned long long)t.phyUnsupported);
    kv(tr("damaged"), "%llu", (unsigned long long)t.blocksBad);
    kv(tr("input"), "%.3f Msps", t.inputRate / 1e6);
    kv(tr("signal time"), "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Frame"), !on ? 0 : a.rx.xlink.dataValid ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::xlinkSummary(a.rx.xlink).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Extra links";
    if (live(a)) l2 = dect2::xlinkSummary(a.rx.xlink);
}

} // namespace

extern const ModeUi kXlinkUi;
const ModeUi kXlinkUi = {
    .sideTitle = "LINKS",
    .tabName = "Links",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = nullptr,
};
