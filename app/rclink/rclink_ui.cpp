// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// RC and mesh link screens: the links heard (Reticulum destinations, ExpressLRS links, MAVLink systems, FrSky and FlySky transmitters), their frames, and a map of
// the MAVLink positions. Receive only. Encrypted or bound payload stays structure only; the ExpressLRS binding phrase is typed here, kept in memory and never saved.
#include "app.h"
#include "i18n/i18n.h"
#include "../adsb/adsb_map.h"
#include "dect2/common/time_fmt.h"
#include "dect2/rclink/rc_rx.h"
#include "dect2/rclink/rc_tel.h"
#include <cmath>
#include <cstdio>
#include <string>

namespace {

struct State {
    int view = 0;
    char phrase[64] = {};
    int domain = 2;              // EU868
    int air = 0;
    adsbmap::View map;
    bool mapFit = true;
};
State S;

bool live(const App& a) { return a.rx.standard == 361; }   // the engine reports its standard code minus one

void banner(App& a) {
    const int mask = a.rx.rclink.protocols;
    ImGui::AlignTextToFramePadding();
    char cl[40]; snprintf(cl, sizeof cl, "%s##rc", tr("Clear"));
    if (ImGui::Button(cl)) a.engine.rclink().clearResults();
    flowNext(10 * gUi);
    if (ImGui::TreeNodeEx(trw("Advanced"), ImGuiTreeNodeFlags_SpanAvailWidth)) {
        if (mask & dect2::rclink::Elrs) {
            ImGui::SetNextItemWidth(220 * gUi);
            if (ImGui::InputTextWithHint("##rcphrase", tr("binding phrase (optional)"), S.phrase, sizeof S.phrase, ImGuiInputTextFlags_Password)) a.engine.rclink().setBindingPhrase(S.phrase);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Your own ExpressLRS binding phrase. Without it only the link structure is shown (SYNC frames, rate and hop index); with it the RC channels of that link are unpacked. It stays in memory and is not saved."));
            const auto& doms = dect2::rcElrsDomains();
            ImGui::SetNextItemWidth(120 * gUi);
            if (ImGui::BeginCombo(tr("Domain"), doms[S.domain].name)) {
                for (int i = 0; i < (int)doms.size(); i++) if (ImGui::Selectable(doms[i].name, i == S.domain)) { S.domain = i; a.engine.rclink().setElrsDomain(i); if (doms[i].startHz > 0 && doms[i].stopHz > doms[i].startHz) tuneFreq(a, (doms[i].startHz + doms[i].stopHz) / 2e6); }   // a new domain retunes to the middle of its band
                ImGui::EndCombo();
            }
        }
        if (mask & dect2::rclink::Sik) {
            static const int rates[] = {0, 2, 4, 8, 16, 19, 24, 32, 48, 64, 96, 128, 192, 250};
            char lb[32]; snprintf(lb, sizeof lb, S.air ? "%d kbit/s" : "%s", S.air ? S.air : 0); if (!S.air) snprintf(lb, sizeof lb, "%s", tr("Auto"));
            ImGui::SetNextItemWidth(120 * gUi);
            if (ImGui::BeginCombo(tr("Air speed"), lb)) {
                for (int r : rates) {
                    char t[32]; if (r) snprintf(t, sizeof t, "%d kbit/s", r); else snprintf(t, sizeof t, "%s", tr("Auto"));
                    if (ImGui::Selectable(t, r == S.air)) { S.air = r; a.engine.rclink().setSikAirRate(r); }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::TreePop();
    }
    flowEnd();
}

void framesView(App& a) {
    const auto& fr = a.rx.rclink.frames;
    if (!ImGui::BeginTable("##rcf", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn(tr("Link")); ImGui::TableSetupColumn(tr("Who")); ImGui::TableSetupColumn(tr("Type"));
    ImGui::TableSetupColumn(tr("Content"), ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn(tr("MHz")); ImGui::TableSetupColumn("SNR");
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    for (const auto& f : fr) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(f.utc).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%s", dect2::rclink::protoName(f.proto));
        ImGui::TableNextColumn(); ImGui::Text("%s", f.id.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%s", f.type.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(f.encrypted ? (f.detail + "  [" + tr("structure only") + "]").c_str() : f.detail.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.3f", f.freqHz / 1e6);
        ImGui::TableNextColumn(); ImGui::Text("%.0f", f.snrDb);
    }
    ImGui::PopFont();
    ImGui::EndTable();
}

void linksView(App& a) {
    const auto& dv = a.rx.rclink.devices;
    if (!ImGui::BeginTable("##rcd", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Link")); ImGui::TableSetupColumn(tr("Who")); ImGui::TableSetupColumn(tr("Last"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(tr("Position")); ImGui::TableSetupColumn(tr("Heard"));
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    for (const auto& d : dv) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%s", dect2::rclink::protoName(d.proto));
        ImGui::TableNextColumn(); ImGui::Text("%s", d.id.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%s: %s", d.lastType.c_str(), d.lastDetail.c_str());
        ImGui::TableNextColumn(); if (d.hasPos) ImGui::Text("%.5f %.5f %.0f m", d.lat, d.lon, d.altM);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s, %llux", dect2::timefmt::clock(d.lastSeen).c_str(), (unsigned long long)d.count);
    }
    ImGui::PopFont();
    ImGui::EndTable();
    if (dv.empty()) ImGui::TextDisabled("%s", tr("no links heard yet"));
}

void mapTab(App& a) {
    const ImVec2 size = ImGui::GetContentRegionAvail(), p0 = ImGui::GetCursorScreenPos();
    if (S.mapFit)
        for (const auto& d : a.rx.rclink.devices) if (d.hasPos) { S.map.lat = d.lat; S.map.lon = d.lon; S.map.zoom = 13; S.mapFit = false; break; }
    adsbmap::draw(S.map, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    for (const auto& d : a.rx.rclink.devices) {
        if (!d.hasPos) continue;
        const ImVec2 q = adsbmap::project(d.lat, d.lon);
        dl->AddCircleFilled(q, 6 * gUi, IM_COL32(230, 150, 60, 255), 20);
        dl->AddCircle(q, 6 * gUi, IM_COL32(20, 24, 26, 220), 20, 1.2f);
        dl->AddText(ImVec2(q.x + 10 * gUi, q.y - 7 * gUi), IM_COL32(230, 233, 236, 255), d.id.c_str());
    }
    dl->PopClipRect();
}

void tab(App& a) {
    banner(a);
    ImGui::Spacing();
    subNav("rcv", S.view, {"Links", "Frames", "Map"});
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    if (S.view == 0) linksView(a); else if (S.view == 1) framesView(a); else mapTab(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const auto& dv = a.rx.rclink.devices;
    if (!ImGui::BeginTable("##rcl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
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
    const dect2::RcTelemetry& t = a.rx.rclink;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv(tr("state"), "%s", dect2::rcSummary(t).c_str());
    kv(tr("frames"), "%llu", (unsigned long long)t.blocksOk);
    kv(tr("bursts"), "%llu", (unsigned long long)t.bursts);
    kv(tr("LoRa preambles"), "%llu", (unsigned long long)t.loraPreambles);
    kv(tr("input"), "%.3f Msps", t.inputRate / 1e6);
    kv(tr("signal time"), "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Burst"), !on ? 0 : a.rx.rclink.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp(tr("Frame"), !on ? 0 : a.rx.rclink.dataValid ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::rcSummary(a.rx.rclink).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "RC and mesh links";
    if (live(a)) l2 = dect2::rcSummary(a.rx.rclink);
}

} // namespace

extern const ModeUi kRcLinkUi;
const ModeUi kRcLinkUi = {
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
