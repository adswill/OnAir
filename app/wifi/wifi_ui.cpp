// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi network screens: the network table (SSID, hidden ones as hidden, BSSID and vendor, band, channel, width, standard, security, level with a
// short history, first and last seen, beacons, interval, capabilities) with name, level, band and security filters, the beacon log (beacons,
// probe responses, FILS discovery), and the channels view. The band selector, channel list and hop scan are in wifi_ui_scan.cpp, the channel
// occupancy chart in wifi_ui_charts.cpp.
// Passive: only the beacons, probe responses and FILS discovery frames access points broadcast openly are read; nothing is sent, probed, joined or decrypted.
#include "dect2/common/time_fmt.h"
#include "wifi_ui_int.h"
#include "ui/ui_common.h"
#include "dect2/wifi/wifi_chan.h"
#include "dect2/wifi/wifi_proto.h"
#include "dect2/wifi/wifi_rx.h"
#include "dect2/wifi/wifi_tel.h"
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

using wfui::S;
using wfui::live;

std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

bool passes(const dect2::WifiAp& d) {
    if (d.rssi < S.minRssi) return false;
    if (S.bandFilter >= 0 && d.band != S.bandFilter) return false;
    if (S.openOnly && d.security != dect2::wifi::Open && d.security != dect2::wifi::Owe) return false;
    if (!S.filter[0]) return true;
    const std::string f = lower(S.filter);
    return lower(d.ssid).find(f) != std::string::npos || lower(d.bssid).find(f) != std::string::npos || lower(d.vendor).find(f) != std::string::npos ||
           lower(dect2::wifi::securityName(d.security, d.enterprise)).find(f) != std::string::npos;
}

ImVec4 securityColour(int s) {
    if (s == dect2::wifi::Open || s == dect2::wifi::Wep) return pal::warnAmber();
    if (s == dect2::wifi::Wpa3 || s == dect2::wifi::Wpa2Wpa3) return pal::okGreen();
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

void passiveNote() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Passive: only the beacons, probe responses and FILS discovery frames that access points broadcast are read. Nothing is transmitted, joined or decrypted."));
    ImGui::PopTextWrapPos();
}

void filters(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Filter"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(160 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##wff", "SSID, BSSID, vendor, security", S.filter, sizeof S.filter);
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Level above"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(120 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::SliderFloat("##wfr", &S.minRssi, -120, 0, "%.0f dBFS");
    flowNext(10 * gUi);
    ImGui::Checkbox("open only", &S.openOnly);
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Band")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(80 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##wfbf", S.bandFilter < 0 ? tr("all") : dect2::wifi::bandName(S.bandFilter))) {
        if (ImGui::Selectable(tr("all"), S.bandFilter < 0)) S.bandFilter = -1;
        for (int b = 0; b < 3; b++) if (ImGui::Selectable(dect2::wifi::bandName(b), S.bandFilter == b)) S.bandFilter = b;
        ImGui::EndCombo();
    }
    flowNext(10 * gUi);
    if (ImGui::Button("Clear")) a.engine.wifi().clearNetworks();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forget every network and beacon heard so far");
    flowBreak();
}

void networksView(App& a) {
    passiveNote();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the networks around you"); return; }
    filters(a);
    const dect2::WifiTelemetry& t = a.rx.wifi;
    if (!ImGui::BeginTable("##wfnet", 14, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("SSID"); ImGui::TableSetupColumn("BSSID"); ImGui::TableSetupColumn("Vendor"); ImGui::TableSetupColumn(tr("Band")); ImGui::TableSetupColumn("Ch");
    ImGui::TableSetupColumn(tr("Width")); ImGui::TableSetupColumn(tr("Std")); ImGui::TableSetupColumn("Security"); ImGui::TableSetupColumn("Level");
    ImGui::TableSetupColumn(tr("First seen")); ImGui::TableSetupColumn("Last seen"); ImGui::TableSetupColumn("Beacons"); ImGui::TableSetupColumn("Interval");
    ImGui::TableSetupColumn(tr("Details"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    int shown = 0;
    for (const auto& d : t.aps) {
        if (!passes(d)) continue;
        shown++;
        ImGui::TableNextRow();
        const double ago = std::max(0.0, t.timeSec - d.lastSec);
        const ImVec4 c = ago < 3 ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::TableNextColumn();
        if (d.ssid.empty()) ImGui::TextDisabled("(hidden)"); else ImGui::TextColored(c, "%s", d.ssid.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s%s\nfirst heard %s%s%s%s\ncapabilities %04X", d.hidden ? "hidden network: its beacons carry no name" : "network name from its beacons",
                              d.hidden && !d.ssid.empty() ? " (the name came from a probe response)" : "", clockText(d.firstWall).c_str(), dect2::timefmt::sfx(),
                              d.probeResps ? "\nprobe responses: " : "", d.probeResps ? std::to_string(d.probeResps).c_str() : "", d.cap);
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(d.bssid.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(d.vendor == "local" ? "(local)" : d.vendor.c_str());
        if (d.vendor == "local" && ImGui::IsItemHovered()) ImGui::SetTooltip("A locally administered BSSID: usually one of several networks of one access point, or a phone hotspot");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::wifi::bandName(d.band));
        ImGui::TableNextColumn(); if (d.channel) ImGui::Text("%d", d.channel); else ImGui::TextDisabled("-");
        if (d.heardOn && d.channel && d.heardOn != d.channel && ImGui::IsItemHovered()) ImGui::SetTooltip("heard on channel %d (the channels overlap)", d.heardOn);
        ImGui::TableNextColumn(); if (d.widthMhz > 20 || !d.standard.empty()) ImGui::Text("%d MHz", d.widthMhz); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        if (d.standard.empty()) ImGui::TextDisabled("-"); else ImGui::TextUnformatted(wfui::stdWord(d.standard));
        if (!d.standard.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("802.11 %s", d.standard.c_str());
        ImGui::TableNextColumn(); ImGui::TextColored(securityColour(d.security), "%s", dect2::wifi::securityName(d.security, d.enterprise));
        if ((!d.ciphers.empty() || !d.akms.empty()) && ImGui::IsItemHovered())
            ImGui::SetTooltip("ciphers: %s\nkey management: %s\nprotected management frames: %s", d.ciphers.c_str(), d.akms.c_str(), d.pmf == 2 ? "required" : d.pmf == 1 ? "capable" : "no");
        ImGui::TableNextColumn();
        ImGui::Text("%4.0f", d.rssi);
        if (d.rssiHist.size() >= 2) {
            ImGui::SameLine(0, 4 * gUi);
            ImGui::PushID(&d);   // one sparkline per row: each needs its own ID
            ImGui::PlotLines("##h", d.rssiHist.data(), (int)d.rssiHist.size(), 0, nullptr, -110, -10, ImVec2(48 * gUi, ImGui::GetTextLineHeight()));
            ImGui::PopID();
        }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clockText(d.firstWall).c_str());
        ImGui::TableNextColumn(); ImGui::Text(ago < 1 ? "now" : "%.0f s ago", ago);
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)d.beacons);
        ImGui::TableNextColumn(); if (d.interval) ImGui::Text("%d TU", d.interval); else ImGui::TextDisabled("-");
        if (d.interval && ImGui::IsItemHovered()) ImGui::SetTooltip("%.1f ms between beacons", d.interval * 1.024);
        ImGui::TableNextColumn();
        std::string det = d.caps;
        auto add = [&](const std::string& x) { if (!x.empty()) { if (!det.empty()) det += "; "; det += x; } };
        if (!d.akms.empty()) add(d.akms + (d.pmf == 2 ? ", PMF required" : d.pmf == 1 ? ", PMF" : ""));
        if (!d.country.empty()) add("country " + d.country);
        if (d.powerConstraint >= 0) add("power constraint " + std::to_string(d.powerConstraint) + " dB");
        if (d.mdid >= 0) { char m[24]; snprintf(m, sizeof m, "mobility domain %04X", d.mdid); add(m); }
        if (d.he6Reg >= 0) add(d.he6Reg == 0 ? "indoor AP" : d.he6Reg == 1 ? "standard power AP" : "very low power AP");
        if (d.fils) add("FILS discovery");
        if (d.unsolicited) add("unsolicited probe responses");
        if (!d.neighbors.empty()) add("neighbors: " + d.neighbors);
        if (det.empty()) det = d.rates.empty() ? "" : "rates " + d.rates;
        ImGui::TextUnformatted(det.c_str());
    }
    ImGui::EndTable();
    if (t.aps.empty()) ImGui::TextDisabled("nothing heard yet");
    else if (!shown) ImGui::TextDisabled("no network passes the filter");
}

void beaconsView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the beacons"); return; }
    const auto& log = a.rx.wifi.log;
    if (ImGui::Button("Copy")) {
        std::string txt;
        for (const auto& p : log) {
            char b[600];
            snprintf(b, sizeof b, "%s\t%d\t%s\t%.0f\t%d\t%s\n", clockText(p.wallTime).c_str(), p.channel, p.bssid.c_str(), p.rssi, p.len, p.info.c_str());
            txt += b;
        }
        ImGui::SetClipboardText(txt.c_str());
    }
    if (!ImGui::BeginTable("##wflog", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(dect2::timefmt::headTime()); ImGui::TableSetupColumn("Ch"); ImGui::TableSetupColumn("BSSID"); ImGui::TableSetupColumn("Level");
    ImGui::TableSetupColumn("Bytes"); ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGuiListClipper clip;
    clip.Begin((int)log.size());
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const auto& p = log[(size_t)i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clockText(p.wallTime).c_str());
            ImGui::TableNextColumn(); if (p.channel) ImGui::Text("%d", p.channel); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(p.bssid.c_str()); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::Text("%.0f", p.rssi);
            ImGui::TableNextColumn(); ImGui::Text("%d", p.len);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.info.c_str());
        }
    ImGui::EndTable();
    if (log.empty()) ImGui::TextDisabled("no beacons yet");
}

// Stations probing and talking, deauthentication floods, handshakes counted only. Addresses are hashed per run, so a client's tag says nothing about the device.
void clientsView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the clients"); return; }
    const dect2::WifiTelemetry& t = a.rx.wifi;
    ImGui::TextDisabled("%s: %llu, %s: %llu", tr("deauth and disassociation frames"), (unsigned long long)t.deauthFrames, tr("WPA handshake frames (counted only, not captured)"), (unsigned long long)t.handshakeFrames);
    for (const auto& al : t.floodAlarms) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s %s (%d %s)", tr("Deauthentication flood at"), al.bssid.c_str(), al.frames, tr("frames in 10 s"));
    if (!ImGui::BeginTable("##wfc", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn(tr("Client")); ImGui::TableSetupColumn(tr("Network")); ImGui::TableSetupColumn(tr("Frames")); ImGui::TableSetupColumn(tr("Probing for"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& c : t.clients) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.tag.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.withAp.empty() ? "-" : c.withAp.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)(c.probeFrames + c.dataFrames));
        ImGui::TableNextColumn();
        std::string pr; for (const auto& s : c.probes) { if (!pr.empty()) pr += ", "; pr += s; }
        ImGui::TextUnformatted(pr.empty() ? "-" : pr.c_str());
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------- hooks

void tick(App& a) { wfui::hopTick(a); }

void tuner(App& a, bool& retune) { wfui::tunerControls(a, retune); }

void tab(App& a) {
    subNav("wfv", S.view, {"Networks", "Beacons", tr("Channels"), tr("Clients")});
    if (S.view == 0) networksView(a); else if (S.view == 1) beaconsView(a); else if (S.view == 3) clientsView(a); else wfui::channelsView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::WifiTelemetry& t = a.rx.wifi;
    if (!ImGui::BeginTable("##wfl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Network", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Level");
    for (const auto& d : t.aps) {
        if (!passes(d)) continue;
        ImGui::TableNextRow();
        const bool recent = t.timeSec - d.lastSec < 3;
        ImGui::TableNextColumn();
        if (d.ssid.empty()) ImGui::TextColored(pal::grey(0.6f), "(hidden)");
        else ImGui::TextColored(recent ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%s", d.ssid.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s %s\nchannel %d, %s", d.bssid.c_str(), d.vendor.c_str(), d.channel, dect2::wifi::securityName(d.security, d.enterprise));
        ImGui::TableNextColumn(); ImGui::TextDisabled("%.0f", d.rssi);
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::WifiTelemetry& t = a.rx.wifi;
    kvRow(a, "state", "%s", dect2::wifiSummary(t).c_str());
    if (t.channel) kvRow(a, "channel", "%s ch %d (%.0f MHz)%s%s", dect2::wifi::bandName(t.band), t.channel, dect2::wifi::bandChannelMhz(t.band, t.channel), S.hop ? ", hopping" : "",
                         t.subChannels > 1 ? (", " + std::to_string(t.subChannels) + " channels at once").c_str() : "");
    kvRow(a, "signal", "symbol SNR %.1f dB, offset %+.1f kHz", t.snrDb, t.cfoHz / 1e3);
    kvRow(a, "frames", "%llu good FCS, %llu bad FCS", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kvRow(a, "PLCP headers", "%llu good, %llu bad CRC, %llu at 2 - 11 Mbit/s (not decoded), %llu preambles", (unsigned long long)t.headersOk, (unsigned long long)t.headersBad,
          (unsigned long long)t.otherRate, (unsigned long long)t.sfdHits);
    kvRow(a, "by type", "beacons %llu, probe responses %llu, other management %llu, control %llu, data %llu (counted only)", (unsigned long long)t.beacons,
          (unsigned long long)t.probeResps, (unsigned long long)(t.perType[0] - std::min(t.perType[0], t.beacons + t.probeResps)), (unsigned long long)t.perType[1],
          (unsigned long long)t.perType[2]);
    kvRow(a, "OFDM", "%llu preambles, %llu SIGNAL good / %llu bad, %llu at other rates, %llu good frames, %llu bad FCS, %llu duplicates on secondary channels, %llu FILS",
          (unsigned long long)t.ofdmTriggers, (unsigned long long)t.ofdmSignalOk, (unsigned long long)t.ofdmSignalBad, (unsigned long long)t.ofdmOther,
          (unsigned long long)t.ofdmFrames, (unsigned long long)t.ofdmBadFcs, (unsigned long long)t.dupFrames, (unsigned long long)t.filsFrames);
    kvRow(a, "input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kvRow(a, "signal time", "%.1f s", t.timeSec);
    ImGui::Spacing();
    passiveNote();
}

void status(App& a) {
    const bool on = live(a);
    StatusPanel panel;
    statusIqLamp(a);
    lamp("Beacons", !on ? 0 : a.rx.wifi.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    statusState(a, on, [&] { return dect2::wifiSummary(a.rx.wifi); });
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Wi-Fi networks";
    if (live(a)) l2 = dect2::wifiSummary(a.rx.wifi);
}

} // namespace

extern const ModeUi kWifiUi;
const ModeUi kWifiUi = {
    .sideTitle = "NETWORKS",
    .tabName = "Networks",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .tick = tick,
};
