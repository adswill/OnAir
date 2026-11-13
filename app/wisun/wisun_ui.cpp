// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-SUN FAN screens: the network list (PAN ID, network name, border router, size, schedule, PHY modes), the node list (EUI-64, vendor, role,
// routing cost, frame types, channels, schedule, EAPOL), the link list (unicast frames and the levels the acks report), the live frame log and the
// hop chart (every frame by time and frequency, coloured by sender), and the band plan picker (every region of the Wi-SUN band table).
// Receive only: nothing is ever sent, joined or answered; PAN configurations and data are encrypted (keys from an EAP-TLS exchange) and stay so.
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "dect2/wisun/wisun_proto.h"
#include "dect2/wisun/wisun_rx.h"
#include "dect2/wisun/wisun_tel.h"
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
    int view = 0;                        // 0 networks, 1 nodes, 2 links, 3 frames, 4 hops
    bool loaded = false;
    std::string band;                    // the band plan's id (wisun::bandPlans()), "" automatic (by the frequency)
    char filter[64] = "";
    float minRssi = -120;
    std::string sel;                     // the node clicked: the log and the links show only its frames
    int selPan = -1;                     // the PAN clicked: the node list and the log show only it
    float hopSecs = 30;                  // the hop chart's time span
    bool colourByType = false;
};
State S;

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.band = plat::prefs().getS("wsBand", "");
    if (S.band.empty()) {   // the earlier setting: 1 ANZ, 2 NA
        const int old = (int)plat::prefs().getI("wsRegion", 0);
        S.band = old == 1 ? "ANZ" : old == 2 ? "NA" : "";
    }
    if (!S.band.empty() && !dect2::wisun::bandPlan(S.band)) S.band.clear();
}

// The band plan's channel plans, one line each: "400 kHz x 32 from 915.4 MHz: 100, 150 kbit/s"
std::string plansText(const dect2::wisun::BandPlan& b) {
    std::string s;
    char t[160];
    snprintf(t, sizeof t, "%s, %.1f - %.1f MHz, regulatory domain 0x%02X", b.name, b.loMhz, b.hiMhz, b.regDomain);
    s = t;
    if (b.gapHiMhz > b.gapLoMhz) { snprintf(t, sizeof t, " (no channels %.1f - %.1f MHz)", b.gapLoMhz, b.gapHiMhz); s += t; }
    for (const auto& p : dect2::wisun::plansOf(b.id)) {
        int n = 0;
        for (int k = 0; k < p.channels; k++) n += dect2::wisun::channelUsable(p, k);
        snprintf(t, sizeof t, "\n  %g kHz x %d from %.3f MHz:", p.spacingKhz, n, p.ch0Mhz);
        s += t;
        for (const auto& r : p.rates) {
            if (r.bitRate <= 0) continue;
            snprintf(t, sizeof t, " %d kbit/s (h %g)%s", r.bitRate / 1000, r.modIndex, r.bitRate > 150000 ? " not decoded" : "");
            s += t;
        }
    }
    return s;
}

void applyBand(App& a) { a.engine.wisun().setRegion(S.band); }

bool live(const App& a) { return a.rx.standard == 85; }   // the engine reports its standard code minus one

std::string clock(int64_t t) {
    if (t <= 0) return "-";
    return dect2::timefmt::clock(t);
}

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string panText(int p) { if (p < 0) return "-"; char b[8]; snprintf(b, sizeof b, "0x%04X", p); return b; }

// The colours of the hop chart: one per sender (its EUI hashed, WsHop::node) or one per frame type
ImU32 nodeColour(int i, int alpha = 230) {
    static const int c[12][3] = {{230, 90, 80}, {90, 170, 230}, {120, 200, 100}, {240, 180, 60}, {180, 120, 220}, {80, 200, 190},
                                 {230, 130, 180}, {170, 170, 90}, {120, 140, 240}, {240, 140, 90}, {150, 210, 150}, {200, 200, 200}};
    if (i < 0) return IM_COL32(150, 150, 150, alpha);
    return IM_COL32(c[i % 12][0], c[i % 12][1], c[i % 12][2], alpha);
}

bool passes(const dect2::WsNode& d) {
    if (d.rssi < S.minRssi) return false;
    if (S.selPan >= 0 && d.pan != S.selPan) return false;
    if (!S.filter[0]) return true;
    const std::string f = lower(S.filter);
    return lower(d.eui).find(f) != std::string::npos || lower(d.vendor).find(f) != std::string::npos || lower(d.role).find(f) != std::string::npos ||
           lower(panText(d.pan)).find(f) != std::string::npos;
}

void receiveNote() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Receive only: OnAir never transmits, joins or answers a network. PAN configurations and data are encrypted (the keys come from an "
                        "EAP-TLS exchange) and are shown as encrypted; what is sent in the clear is decoded: headers and addresses, PAN adverts and "
                        "solicits (network name, size, routing cost, schedules), EAPOL and the levels acks report. Nodes hop over every channel: "
                        "16 Msps or more takes in all of 915 - 928 MHz at once.");
    ImGui::PopTextWrapPos();
}

void filters(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Filter"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(160 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##wsf", "EUI-64, vendor, role, PAN", S.filter, sizeof S.filter);
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Level above"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(120 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::SliderFloat("##wsr", &S.minRssi, -120, 0, "%.0f dBFS");
    flowNext(10 * gUi);
    if (S.selPan >= 0) {
        ImGui::TextColored(pal::okGreen(), "PAN %s only", panText(S.selPan).c_str());
        ImGui::SameLine(0, 5 * gUi);
        if (ImGui::SmallButton("Show all")) S.selPan = -1;
        flowNext(10 * gUi);
    }
    if (ImGui::Button("Clear")) a.engine.wisun().clearResults();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forget every network, node, link and frame heard so far");
    flowBreak();
}

std::string notLive(const App& a, const char* what) { return a.engine.running() ? "starting" : std::string("start the receiver to see ") + what; }

void networksView(App& a) {
    receiveNote();
    if (!live(a)) { ImGui::TextDisabled("%s", notLive(a, "the networks around you").c_str()); return; }
    const dect2::WisunTelemetry& t = a.rx.wisun;
    if (t.ppsHist.size() >= 2) {
        ImGui::TextDisabled("Frames per second");
        ImGui::PlotLines("##wspps", t.ppsHist.data(), (int)t.ppsHist.size(), 0, nullptr, 0, FLT_MAX, ImVec2(ImGui::GetContentRegionAvail().x, 44 * gUi));
    }
    if (!ImGui::BeginTable("##wspan", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("PAN ID"); ImGui::TableSetupColumn("Network name"); ImGui::TableSetupColumn("Border router"); ImGui::TableSetupColumn("Size");
    ImGui::TableSetupColumn("Heard"); ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("Frames"); ImGui::TableSetupColumn("Last seen");
    ImGui::TableSetupColumn("Schedule", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& p : t.pans) {
        ImGui::TableNextRow();
        ImGui::PushID((int)p.pan);
        const double ago = std::max(0.0, t.timeSec - p.lastSec);
        const ImVec4 c = ago < 5 ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::TableNextColumn();
        if (ImGui::Selectable("##row", S.selPan == p.pan, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.selPan = S.selPan == p.pan ? -1 : p.pan;
        ImGui::SameLine(0, 0);
        ImGui::PushFont(a.mono, 0); ImGui::TextColored(c, "%s", panText(p.pan).c_str()); ImGui::PopFont();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s%s%s%s%s\nclick to show only this network's nodes and frames", p.tpsVersion == 2 ? "Wi-SUN FAN 1.1" : p.tpsVersion == 1 ? "Wi-SUN FAN 1.0" : "Wi-SUN FAN",
                              p.panVersion >= 0 ? ", PAN version " : "", p.panVersion >= 0 ? std::to_string(p.panVersion).c_str() : "",
                              p.phyModes.empty() ? "" : "\nPHY operating modes ", p.phyModes.c_str());
        ImGui::TableNextColumn(); if (p.netName.empty()) ImGui::TextDisabled("not advertised yet"); else ImGui::TextUnformatted(p.netName.c_str());
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(p.borderRouter.empty() ? "-" : p.borderRouter.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); if (p.panSize >= 0) ImGui::Text("%d", p.panSize); else ImGui::TextDisabled("-");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("the PAN size the border router advertises (PAN-IE)");
        ImGui::TableNextColumn(); ImGui::Text("%d", p.nodes);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("nodes heard here");
        ImGui::TableNextColumn(); ImGui::Text("%.0f", p.rssi);
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)p.packets);
        ImGui::TableNextColumn(); ImGui::Text(ago < 1 ? "now" : "%.0f s ago", ago);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", p.unicast.empty() ? "-" : p.unicast.c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (t.pans.empty()) ImGui::TextDisabled(t.blocksOk ? "frames heard, no PAN ID yet (PAN adverts come every few minutes)" : "nothing heard yet");
}

std::string uttCounts(const dect2::WsNode& d) {
    std::string s;
    for (int k = 0; k < dect2::kWsUttTypes; k++) {
        if (!d.perUtt[k]) continue;
        char b[32];
        snprintf(b, sizeof b, "%s%s %llu", s.empty() ? "" : ", ", dect2::wisun::uttShort(k), (unsigned long long)d.perUtt[k]);
        s += b;
    }
    return s;
}

std::string channelsText(const dect2::WsNode& d, const dect2::WisunTelemetry& t) {
    int n = 0;
    for (uint8_t c : d.channels) n += c;
    char b[48];
    snprintf(b, sizeof b, "%d of %d", n, t.channelsCovered);
    return b;
}

void nodesView(App& a) {
    receiveNote();
    if (!live(a)) { ImGui::TextDisabled("%s", notLive(a, "the nodes around you").c_str()); return; }
    filters(a);
    const dect2::WisunTelemetry& t = a.rx.wisun;
    if (!ImGui::BeginTable("##wsnode", 10, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("EUI-64"); ImGui::TableSetupColumn("Vendor"); ImGui::TableSetupColumn("Role"); ImGui::TableSetupColumn("PAN"); ImGui::TableSetupColumn("Cost");
    ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("Frames"); ImGui::TableSetupColumn("Channels"); ImGui::TableSetupColumn("Last seen");
    ImGui::TableSetupColumn("Frame types / last frame", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    int shown = 0;
    for (const auto& d : t.nodes) {
        if (!passes(d)) continue;
        shown++;
        ImGui::TableNextRow();
        ImGui::PushID(d.eui.c_str());
        const double ago = std::max(0.0, t.timeSec - d.lastSec);
        ImGui::TableNextColumn();
        if (ImGui::Selectable("##row", S.sel == d.eui, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.sel = S.sel == d.eui ? std::string() : d.eui;
        ImGui::SameLine(0, 0);
        {   // a swatch in the node's hop chart colour
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float h = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + h * 0.25f), ImVec2(p.x + h * 0.5f, p.y + h * 0.75f), nodeColour((int)(d.euiNum ? [&] {
                uint64_t e = d.euiNum; e ^= e >> 29; e *= 0xBF58476D1CE4E5B9ull; e ^= e >> 32; return (int)(e % 12); }() : -1)));
            ImGui::Dummy(ImVec2(h * 0.7f, h)); ImGui::SameLine(0, 0);
        }
        ImGui::PushFont(a.mono, 0); ImGui::TextColored(ago < 5 ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", d.eui.c_str()); ImGui::PopFont();
        if (ImGui::IsItemHovered()) {
            std::string tip = "click to show only this node's frames and links";
            if (!d.unicast.empty()) tip = "unicast schedule: " + d.unicast + "\n" + tip;
            if (!d.broadcast.empty()) tip = "broadcast schedule: " + d.broadcast + "\n" + tip;
            if (!d.phyModes.empty()) tip = "PHY operating modes: " + d.phyModes + "\n" + tip;
            if (!d.authenticator.empty()) tip = "joining through authenticator " + d.authenticator + (d.eapState.empty() ? "" : " (" + d.eapState + ")") + "\n" + tip;
            if (d.encrypted) tip = "security frame counter " + std::to_string(d.frameCounter) + "\n" + tip;
            ImGui::SetTooltip("%s", tip.c_str());
        }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(d.vendor.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(d.role.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(panText(d.pan).c_str());
        ImGui::TableNextColumn(); if (d.routingCost >= 0) ImGui::Text("%d", d.routingCost); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (d.packets) ImGui::Text("%.0f", d.rssi); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)d.packets);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(channelsText(d, t).c_str());
        ImGui::TableNextColumn(); if (d.packets) ImGui::Text(ago < 1 ? "now" : "%.0f s ago", ago); else ImGui::TextDisabled("addressed only");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(uttCounts(d).c_str()); ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("%s", d.last.c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (t.nodes.empty()) ImGui::TextDisabled("nothing heard yet");
    else if (!shown) ImGui::TextDisabled("no node passes the filter");
}

void linksView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", notLive(a, "the links").c_str()); return; }
    const dect2::WisunTelemetry& t = a.rx.wisun;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Unicast frames from one node to another, and the enhanced acks coming back with the level the receiving node measured (RSL-IE): "
                        "the mesh's links as far as they are heard from here.");
    ImGui::PopTextWrapPos();
    if (!S.sel.empty()) {
        ImGui::TextColored(pal::okGreen(), "Showing %s only", S.sel.c_str());
        ImGui::SameLine(0, 8 * gUi);
        if (ImGui::SmallButton("Show all")) S.sel.clear();
    }
    if (!ImGui::BeginTable("##wslink", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("From"); ImGui::TableSetupColumn("To"); ImGui::TableSetupColumn("Frames"); ImGui::TableSetupColumn("Acks");
    ImGui::TableSetupColumn("RSL"); ImGui::TableSetupColumn("Level here"); ImGui::TableSetupColumn("Last seen", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& l : t.links) {
        if (!S.sel.empty() && l.src != S.sel && l.dst != S.sel) continue;
        ImGui::TableNextRow();
        const double ago = std::max(0.0, t.timeSec - l.lastSec);
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(l.src.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(l.dst.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)l.frames);
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)l.acks);
        ImGui::TableNextColumn(); if (l.rslDbm > -999) ImGui::Text("%d dBm", l.rslDbm); else ImGui::TextDisabled("-");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("the level of the 'from' node's frames as the 'to' node measured it (sent back in its acks)");
        ImGui::TableNextColumn(); if (l.frames) ImGui::Text("%.0f dBFS", l.rssi); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::Text(ago < 1 ? "now" : "%.0f s ago", ago);
    }
    ImGui::EndTable();
    if (t.links.empty()) ImGui::TextDisabled("no unicast frame heard yet");
}

void framesView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", notLive(a, "the frames").c_str()); return; }
    const dect2::WisunTelemetry& t = a.rx.wisun;
    const auto& log = t.log;
    if (!S.sel.empty() || S.selPan >= 0) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(pal::okGreen(), "Showing %s only", !S.sel.empty() ? S.sel.c_str() : panText(S.selPan).c_str());
        ImGui::SameLine(0, 8 * gUi);
        if (ImGui::SmallButton("Show all")) { S.sel.clear(); S.selPan = -1; }
    }
    std::vector<size_t> rows;
    for (size_t i = 0; i < log.size(); i++) {
        const auto& f = log[i];
        if (S.selPan >= 0 && f.pan != S.selPan) continue;
        if (!S.sel.empty() && f.src != S.sel && f.dst != S.sel) continue;
        rows.push_back(i);
    }
    if (ImGui::Button("Copy")) {
        std::string txt;
        for (size_t i : rows) {
            const auto& f = log[i];
            char b[700];
            snprintf(b, sizeof b, "%s\t%.3f\t%d\t%d\t%s\t%s > %s\t%d\t%.0f\t%s\t%s\n", clock(f.wallTime).c_str(), f.mhz, f.channel, f.bitRate / 1000, f.type.c_str(),
                     f.src.c_str(), f.dst.c_str(), f.len, f.rssi, f.info.c_str(), f.ies.c_str());
            txt += b;
        }
        ImGui::SetClipboardText(txt.c_str());
    }
    ImGui::SameLine(0, 8 * gUi);
    ImGui::TextDisabled("%.0f frames/s", t.pps);
    if (!ImGui::BeginTable("##wslog", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(dect2::timefmt::headTime()); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("Ch"); ImGui::TableSetupColumn("kbit/s"); ImGui::TableSetupColumn("Type");
    ImGui::TableSetupColumn("Source > destination"); ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGuiListClipper clip;
    clip.Begin((int)rows.size());
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const auto& f = log[rows[(size_t)i]];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(f.wallTime).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.1f", f.mhz);
            ImGui::TableNextColumn(); if (f.channel >= 0) ImGui::Text("%d", f.channel); else ImGui::TextDisabled("-");
            if (ImGui::IsItemHovered() && f.channel >= 0) ImGui::SetTooltip("channel %d of operating class %d", f.channel, f.opClass);
            ImGui::TableNextColumn(); ImGui::Text("%d", f.bitRate / 1000);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(f.type.c_str());
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::Text("%s > %s", f.src.c_str(), f.dst.c_str()); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::Text("%.0f", f.rssi);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.1f dBFS, SNR %.1f dB, %d octets", f.rssi, f.snrDb, f.len);
            ImGui::TableNextColumn();
            if (f.encrypted) ImGui::TextDisabled("%s", f.info.c_str()); else ImGui::TextUnformatted(f.info.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("IEs: %s", f.ies.empty() ? "none" : f.ies.c_str());
        }
    ImGui::EndTable();
    if (log.empty()) ImGui::TextDisabled("no frames yet");
    else if (rows.empty()) ImGui::TextDisabled("no frame passes the selection");
}

// Every frame as a dot: time across, frequency up (the channels the receiver covers), coloured by sender or by frame type
void hopsView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", notLive(a, "the hop chart").c_str()); return; }
    const dect2::WisunTelemetry& t = a.rx.wisun;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Span"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(140 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::SliderFloat("##wshs", &S.hopSecs, 5, 600, "%.0f s", ImGuiSliderFlags_Logarithmic);
    flowNext(10 * gUi);
    ImGui::Checkbox("colour by frame type", &S.colourByType);
    flowBreak();
    const double lo = t.loMhz > 0 ? t.loMhz : 915, hi = t.hiMhz > lo ? t.hiMhz : 928;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 sz(ImGui::GetContentRegionAvail().x, std::max(120.f, ImGui::GetContentRegionAvail().y - 24 * gUi));
    const float left = 56 * gUi;
    const ImVec2 a0(p0.x + left, p0.y), a1(p0.x + sz.x, p0.y + sz.y);
    dl->AddRectFilled(a0, a1, ImGui::GetColorU32(ImGuiCol_FrameBg));
    const double t1 = t.timeSec, t0 = t1 - S.hopSecs;
    auto yOf = [&](double mhz) { return a1.y - (float)((mhz - lo) / (hi - lo)) * (a1.y - a0.y); };
    auto xOf = [&](double s) { return a0.x + (float)((s - t0) / (t1 - t0)) * (a1.x - a0.x); };
    // grid: every MHz
    for (int m = (int)std::ceil(lo); m <= (int)std::floor(hi); m++) {
        const float y = yOf(m);
        dl->AddLine(ImVec2(a0.x, y), ImVec2(a1.x, y), ImGui::GetColorU32(ImGuiCol_Border));
        char b[16]; snprintf(b, sizeof b, "%d", m);
        dl->AddText(ImVec2(p0.x + 4 * gUi, y - ImGui::GetTextLineHeight() / 2), ImGui::GetColorU32(ImGuiCol_TextDisabled), b);
    }
    const float r = std::max(2.f, 2.5f * gUi);
    for (const auto& h : t.hops) {
        if (h.sec < t0 || h.sec > t1) continue;
        const int c = S.colourByType ? h.utt : h.node;
        dl->AddCircleFilled(ImVec2(xOf(h.sec), yOf(h.mhz)), r, nodeColour(c));
    }
    ImGui::Dummy(sz);
    if (S.colourByType) {
        for (int k = 0; k < dect2::kWsUttTypes; k++) {
            if (!t.perUtt[k]) continue;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float h = ImGui::GetTextLineHeight();
            dl->AddCircleFilled(ImVec2(p.x + h / 2, p.y + h / 2), h / 3, nodeColour(k));
            ImGui::Dummy(ImVec2(h, h)); ImGui::SameLine(0, 2 * gUi);
            ImGui::TextUnformatted(dect2::wisun::uttName(k)); flowNext(10 * gUi);
        }
        flowBreak();
    } else ImGui::TextDisabled("one colour per sender (the swatch in the node list); %.0f s across, %.1f - %.1f MHz up", S.hopSecs, lo, hi);
}

// ---------------------------------------------------------------- hooks

void tuner(App& a, bool& retune) {
    loadState();
    namespace ws = dect2::wisun;
    const ws::BandPlan& autoPlan = ws::bandPlanFor(a.freqMhz);
    const ws::BandPlan* cur = S.band.empty() ? &autoPlan : ws::bandPlan(S.band);
    if (!cur) cur = &autoPlan;
    ImGui::TextDisabled("Band plan");
    ImGui::SameLine(0, 5 * gUi);
    char label[96];
    if (S.band.empty()) snprintf(label, sizeof label, "automatic (%s)", autoPlan.id);
    else snprintf(label, sizeof label, "%s  %s", cur->id, cur->name);
    ImGui::SetNextItemWidth(std::min(230 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##wsband", label)) {
        if (ImGui::Selectable("automatic: by the frequency", S.band.empty())) { S.band.clear(); plat::prefs().setS("wsBand", ""); plat::prefs().flush(); applyBand(a); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("ANZ from 914.5 MHz up to 929, North America below, Europe at 863 - 876 MHz, China at 470 - 510 and\n779 - 787 MHz, the worldwide band at 2.4 GHz; any other region by name");
        for (const ws::BandPlan& b : ws::bandPlans()) {
            char e[128];
            snprintf(e, sizeof e, "%-6s %s  %.1f - %.1f MHz", b.id, b.name, b.loMhz, b.hiMhz);
            if (ImGui::Selectable(e, S.band == b.id)) { S.band = b.id; plat::prefs().setS("wsBand", S.band); plat::prefs().flush(); applyBand(a); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", plansText(b).c_str());
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", plansText(*cur).c_str());
    ImGui::SameLine(0, 8 * gUi);
    const double mid = (cur->loMhz + cur->hiMhz) / 2, needMsps = (cur->hiMhz - cur->loMhz) / 0.9;
    if (ImGui::SmallButton("Centre on band")) {
        if (!S.band.empty() || std::fabs(a.freqMhz - mid) > 0.01) { tuneFreq(a, mid); retune = true; }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tune to %.2f MHz, the middle of %s: the whole band at %.0f Msps or more,\nless of it (the channels around the centre) at a lower rate", mid, cur->name, std::ceil(needMsps));
    if (live(a) && a.rx.wisun.loMhz > 0) {
        ImGui::SameLine(0, 8 * gUi);
        const dect2::WisunTelemetry& t = a.rx.wisun;
        ImGui::TextDisabled("covers %.1f - %.1f MHz, %d of %zu channels", t.loMhz, t.hiMhz, t.channelsCovered, t.chanMhz.size());
    } else if (live(a) && a.rx.wisun.inputRate > 0) {
        ImGui::SameLine(0, 8 * gUi);
        ImGui::TextColored(pal::warnAmber(), "no %s channel in the band taken in", cur->id);
    }
}

void tick(App& a) {
    loadState();
    static bool applied = false;
    if (!applied) { applied = true; applyBand(a); }
}

void tab(App& a) {
    subNav("wsv", S.view, {"Networks", "Nodes", "Links", "Frames", "Hops"});
    if (S.view == 0) networksView(a);
    else if (S.view == 1) nodesView(a);
    else if (S.view == 2) linksView(a);
    else if (S.view == 3) framesView(a);
    else hopsView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::WisunTelemetry& t = a.rx.wisun;
    if (!ImGui::BeginTable("##wsl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Network / node", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Level");
    for (const auto& p : t.pans) {
        ImGui::TableNextRow();
        ImGui::PushID((int)p.pan);
        const bool recent = t.timeSec - p.lastSec < 5;
        ImGui::TableNextColumn();
        if (ImGui::Selectable("##row", S.selPan == p.pan, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.selPan = S.selPan == p.pan ? -1 : p.pan;
        ImGui::SameLine(0, 0);
        ImGui::PushFont(a.mono, 0);
        ImGui::TextColored(recent ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s %s", panText(p.pan).c_str(), p.netName.c_str());
        ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%d nodes heard%s%s", p.nodes, p.borderRouter.empty() ? "" : "\nborder router ", p.borderRouter.c_str());
        ImGui::TableNextColumn(); ImGui::TextDisabled("%.0f", p.rssi);
        ImGui::PopID();
    }
    for (const auto& d : t.nodes) {
        if (d.role != "border router" && d.role != "router") continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(d.eui.c_str()); ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s%s", d.role.c_str(), d.vendor.empty() ? "" : ", ", d.vendor.c_str());
        ImGui::TableNextColumn(); ImGui::TextDisabled("%.0f", d.rssi);
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::WisunTelemetry& t = a.rx.wisun;
    kvRow(a, "state", "%s", dect2::wisunSummary(t).c_str());
    kvRow(a, "band plan", "%s %s, %.1f - %.1f MHz", t.region.c_str(), t.bandName.c_str(), t.bandLoMhz, t.bandHiMhz);
    if (t.loMhz > 0) kvRow(a, "covers", "%.1f - %.1f MHz, %d of %zu channels, %d bins of %.1f kHz", t.loMhz, t.hiMhz, t.channelsCovered, t.chanMhz.size(), t.bins, t.binKhz);
    kvRow(a, "signal", "SNR %.1f dB, offset %+.1f kHz", t.snrDb, t.cfoHz / 1e3);
    kvRow(a, "frames", "%llu good, %llu failed FCS, %llu FEC (not decoded), %llu bursts, %llu encrypted", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
          (unsigned long long)t.fecFrames, (unsigned long long)t.bursts, (unsigned long long)t.encryptedFrames);
    std::string ty;
    for (int k = 0; k < dect2::kWsUttTypes; k++)
        if (t.perUtt[k]) { char b[40]; snprintf(b, sizeof b, "%s%s %llu", ty.empty() ? "" : ", ", dect2::wisun::uttShort(k), (unsigned long long)t.perUtt[k]); ty += b; }
    if (t.plain) { char b[40]; snprintf(b, sizeof b, "%splain 802.15.4 %llu", ty.empty() ? "" : ", ", (unsigned long long)t.plain); ty += b; }
    kvRow(a, "by type", "%s", ty.empty() ? "-" : ty.c_str());
    // the busiest channels
    std::vector<std::pair<uint32_t, int>> busy;
    for (size_t i = 0; i < t.perChannel.size(); i++) if (t.perChannel[i]) busy.push_back({t.perChannel[i], (int)i});
    std::sort(busy.rbegin(), busy.rend());
    std::string ch;
    for (size_t i = 0; i < busy.size() && i < 8; i++) {
        char b[40];
        snprintf(b, sizeof b, "%s%.3f: %u", ch.empty() ? "" : ", ", (size_t)busy[i].second < t.chanMhz.size() ? t.chanMhz[(size_t)busy[i].second] : 0.0, busy[i].first);
        ch += b;
    }
    kvRow(a, "busiest (MHz)", "%s", ch.empty() ? "-" : ch.c_str());
    kvRow(a, "input", "%.3f Msps, %.1f dBFS, load %.0f%%", t.inputRate / 1e6, t.levelDb, t.loadPct);
    kvRow(a, "signal time", "%.1f s", t.timeSec);
    ImGui::Spacing();
    receiveNote();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Frames", !on ? 0 : a.rx.wisun.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::wisunSummary(a.rx.wisun).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Wi-SUN FAN";
    if (live(a)) l2 = dect2::wisunSummary(a.rx.wisun);
}

} // namespace

extern const ModeUi kWisunUi;
const ModeUi kWisunUi = {
    .sideTitle = "NETWORKS",
    .tabName = "Wi-SUN",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .tick = tick,
};
