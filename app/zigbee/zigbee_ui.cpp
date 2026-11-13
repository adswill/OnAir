// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Zigbee / IEEE 802.15.4 screens: the PAN list (PAN ID, extended PAN ID, channel, coordinator, devices, level, packets, last seen), the device list
// (short and extended address, vendor from the OUI, role, level), the live frame log (time, channel, frame type, source > destination, length, FCS)
// with a packets-per-second plot, the channel picker (11 - 26) and the channel scan.
// Receive only: nothing is ever sent, joined or answered; encrypted payloads are not decrypted (the network key is not obtainable here), only frames
// sent in the clear are shown in full.
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "ui/topo_view.h"
#include "dect2/zigbee/zigbee_proto.h"
#include "dect2/zigbee/zigbee_rx.h"
#include "dect2/zigbee/zigbee_tel.h"
#include "dect2/zigbee/zigbee_health.h"
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
    int view = 0;                        // 0 networks, 1 devices, 2 frames
    bool loaded = false, scan = false;
    int dwellSec = 5;
    double lastHop = 0;
    char filter[64] = "";
    float minRssi = -120;
    std::string sel;                     // "pan/address" of the device clicked: the frame log shows only its frames
    bool curThread = false;              // the screens being drawn are the Thread mode's
    int selPan = -1;                     // the PAN clicked in the network list: the device list and the log show only it
};
State S;

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.scan = plat::prefs().getB("zbScan", false);
    S.dwellSec = std::max(1, std::min(60, (int)plat::prefs().getI("zbDwell", 5)));
}

bool isThread(const App& a) { return a.family == 244; }   // the Thread mode shares these screens (the same 802.15.4 receiver, frames read as Thread)
const dect2::ZigbeeTelemetry& zt(const App& a) { return isThread(a) ? a.rx.thread : a.rx.zigbee; }
bool live(const App& a) { return a.rx.standard == (isThread(a) ? 245 : 66); }   // the engine reports its standard code minus one

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

std::string panText(uint16_t p) { char b[8]; snprintf(b, sizeof b, "0x%04X", p); return b; }
std::string devKey(const dect2::ZbDevice& d) { return panText(d.pan) + "/" + (d.shortAddr.empty() ? d.extAddr : d.shortAddr); }

bool passes(const dect2::ZbDevice& d) {
    if (d.rssi < S.minRssi) return false;
    if (S.selPan >= 0 && d.pan != S.selPan) return false;
    if (!S.filter[0]) return true;
    const std::string f = lower(S.filter);
    return lower(d.shortAddr).find(f) != std::string::npos || lower(d.extAddr).find(f) != std::string::npos || lower(d.vendor).find(f) != std::string::npos ||
           lower(d.role).find(f) != std::string::npos || lower(panText(d.pan)).find(f) != std::string::npos;
}

void receiveNote() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Receive only: OnAir never transmits, joins or answers a network. Encrypted payloads are not decrypted (the network key is not obtainable here); "
                        "only frames sent in the clear are shown in full: beacons, MAC commands, headers and the few unencrypted messages. Needs a fast radio "
                        "(4 Msps or more, 2.4 GHz: not an RTL-SDR).");
    if (S.curThread)
        ImGui::TextDisabled("Thread: nearly all frames are AES-CCM encrypted at the link layer. For them the 802.15.4 header, the frame counter and the key index are shown "
                            "and the content (6LoWPAN, IPv6, MLE, CoAP) is marked encrypted. Beacons (network name, extended PAN ID), Joiner scans (MLE Discovery) and other "
                            "clear frames are decoded.");
    ImGui::PopTextWrapPos();
}

void filters(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Filter"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(160 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##zbf", "address, vendor, role, PAN", S.filter, sizeof S.filter);
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Level above"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(120 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::SliderFloat("##zbr", &S.minRssi, -120, 0, "%.0f dBFS");
    flowNext(10 * gUi);
    if (S.selPan >= 0) {
        ImGui::TextColored(pal::okGreen(), "PAN %s only", panText((uint16_t)S.selPan).c_str());
        ImGui::SameLine(0, 5 * gUi);
        if (ImGui::SmallButton("Show all")) S.selPan = -1;
        flowNext(10 * gUi);
    }
    if (ImGui::Button("Clear")) a.engine.zigbee().clearResults();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forget every network, device and frame heard so far");
    flowBreak();
}

void networksView(App& a) {
    receiveNote();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the networks around you"); return; }
    const dect2::ZigbeeTelemetry& t = zt(a);
    if (t.ppsHist.size() >= 2) {
        ImGui::TextDisabled("Frames per second");
        ImGui::PlotLines("##zbpps", t.ppsHist.data(), (int)t.ppsHist.size(), 0, nullptr, 0, FLT_MAX, ImVec2(ImGui::GetContentRegionAvail().x, 44 * gUi));
    }
    const bool th = isThread(a);
    if (!ImGui::BeginTable("##zbpan", th ? 9 : 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("PAN ID"); if (th) ImGui::TableSetupColumn("Network name"); ImGui::TableSetupColumn("Extended PAN ID"); ImGui::TableSetupColumn("Ch"); ImGui::TableSetupColumn("Coordinator");
    ImGui::TableSetupColumn("Devices"); ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("Packets"); ImGui::TableSetupColumn("Last seen", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& p : t.pans) {
        ImGui::TableNextRow();
        ImGui::PushID((int)(p.pan * 32 + p.channel));
        const double ago = std::max(0.0, t.timeSec - p.lastSec);
        const ImVec4 c = ago < 3 ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::TableNextColumn();
        if (ImGui::Selectable("##row", S.selPan == p.pan, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.selPan = S.selPan == p.pan ? -1 : p.pan;
        ImGui::SameLine(0, 0);
        ImGui::PushFont(a.mono, 0); ImGui::TextColored(c, "%s", panText(p.pan).c_str()); ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s%s\nclick to show only this network's devices and frames", th ? (p.zigbee ? "Thread network" : "IEEE 802.15.4 network (no Thread header seen)") : p.zigbee ? "Zigbee network" : "IEEE 802.15.4 network (no Zigbee header seen)",
                                                      p.stackProfile >= 0 ? (p.stackProfile == 2 ? ", stack profile 2 (Zigbee PRO)" : p.stackProfile == 1 ? ", stack profile 1" : "") : "", p.joinOpen ? "\njoining is open" : "");
        if (th) { ImGui::TableNextColumn(); if (p.networkName.empty()) ImGui::TextDisabled("-"); else ImGui::TextUnformatted(p.networkName.c_str()); }
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(p.extPan.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); if (p.channel) ImGui::TextUnformatted(dect2::zigbee::channelCodeName(p.channel).c_str()); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(p.coordinator.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%d", p.devices);
        ImGui::TableNextColumn(); ImGui::Text("%.0f", p.rssi);
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)p.packets);
        ImGui::TableNextColumn(); ImGui::Text(ago < 1 ? "now" : "%.0f s ago", ago);
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (t.pans.empty()) ImGui::TextDisabled("nothing heard yet");
}

void devicesView(App& a) {
    receiveNote();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the devices around you"); return; }
    filters(a);
    const dect2::ZigbeeTelemetry& t = zt(a);
    if (!ImGui::BeginTable("##zbdev", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Short"); ImGui::TableSetupColumn("Extended"); ImGui::TableSetupColumn("Vendor"); ImGui::TableSetupColumn("PAN"); ImGui::TableSetupColumn("Role");
    ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("Packets"); ImGui::TableSetupColumn("Last seen / last frame", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    int shown = 0;
    for (const auto& d : t.devices) {
        if (!passes(d)) continue;
        shown++;
        ImGui::TableNextRow();
        const std::string key = devKey(d);
        ImGui::PushID(key.c_str());
        const double ago = std::max(0.0, t.timeSec - d.lastSec);
        const ImVec4 c = ago < 3 ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::TableNextColumn();
        if (ImGui::Selectable("##row", S.sel == key, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.sel = S.sel == key ? std::string() : key;
        ImGui::SameLine(0, 0);
        ImGui::PushFont(a.mono, 0); ImGui::TextColored(c, "%s", d.shortAddr.empty() ? "-" : d.shortAddr.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(d.extAddr.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(d.vendor.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(panText(d.pan).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(d.role.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.0f", d.rssi);
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)d.packets);
        ImGui::TableNextColumn(); ImGui::Text(ago < 1 ? "now" : "%.0f s ago", ago); ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("%s", d.last.c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (t.devices.empty()) ImGui::TextDisabled("nothing heard yet");
    else if (!shown) ImGui::TextDisabled("no device passes the filter");
}

void framesView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the frames"); return; }
    const dect2::ZigbeeTelemetry& t = zt(a);
    const auto& log = t.log;
    if (!S.sel.empty() || S.selPan >= 0) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(pal::okGreen(), "Showing %s only", !S.sel.empty() ? S.sel.c_str() : panText((uint16_t)S.selPan).c_str());
        ImGui::SameLine(0, 8 * gUi);
        if (ImGui::SmallButton("Show all")) { S.sel.clear(); S.selPan = -1; }
    }
    const std::string key = S.sel;
    std::vector<size_t> rows;
    for (size_t i = 0; i < log.size(); i++) {
        const auto& f = log[i];
        if (S.selPan >= 0 && f.pan != S.selPan) continue;
        if (!key.empty()) {
            const std::string pfx = panText(f.pan) + "/";
            if (key != pfx + f.src && key != pfx + f.dst) continue;
        }
        rows.push_back(i);
    }
    if (ImGui::Button("Copy")) {
        std::string txt;
        for (size_t i : rows) {
            const auto& f = log[i];
            char b[600];
            snprintf(b, sizeof b, "%s\t%s\t%s\t%s > %s\t%d\t%s\t%s\n", clock(f.wallTime).c_str(), dect2::zigbee::channelCodeName(f.channel).c_str(), f.type.c_str(), f.src.c_str(), f.dst.c_str(), f.len, f.fcsOk ? "ok" : "bad", f.info.c_str());
            txt += b;
        }
        ImGui::SetClipboardText(txt.c_str());
    }
    ImGui::SameLine(0, 8 * gUi);
    ImGui::TextDisabled("%.0f frames/s", t.pps);
    if (!ImGui::BeginTable("##zblog", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(dect2::timefmt::headTime()); ImGui::TableSetupColumn("Ch"); ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("Source > destination");
    ImGui::TableSetupColumn("Len"); ImGui::TableSetupColumn("FCS"); ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGuiListClipper clip;
    clip.Begin((int)rows.size());
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const auto& f = log[rows[(size_t)i]];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(f.wallTime).c_str());
            ImGui::TableNextColumn(); if (f.channel) ImGui::TextUnformatted(dect2::zigbee::channelCodeName(f.channel).c_str()); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(f.type.c_str());
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::Text("%s > %s", f.src.c_str(), f.dst.c_str()); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::Text("%d", f.len);
            ImGui::TableNextColumn(); if (f.fcsOk) ImGui::TextColored(pal::okGreen(), "ok"); else ImGui::TextDisabled("bad");
            ImGui::TableNextColumn(); if (f.encrypted) ImGui::TextDisabled("%s", f.info.c_str()); else ImGui::TextUnformatted(f.info.c_str());
        }
    ImGui::EndTable();
    if (log.empty()) ImGui::TextDisabled("no frames yet");
    else if (rows.empty()) ImGui::TextDisabled("no frame passes the selection");
}

void hopTo(App& a, int ch) {
    const double mhz = dect2::zigbee::channelCodeMhz(ch);
    a.freqMhz = mhz;
    a.tune.centerHz = mhz * 1e6;
    a.engine.retune(a.tune);   // no reset: the tables stay
}

// Home network health (3.9, docs/SMART5-iot-3.9.md item 14, wired for Zigbee / Thread in docs/SMART5-UI-3.9.md): for the one PAN the user picks
// as theirs, per node: MAC retransmissions (the same sequence number again within 3 s), requested acknowledgements that never came, the level
// trend and silence (dect2/zwave/homenet_health.h). An 802.15.4 acknowledgement carries only the sequence number, so it is matched to the
// frame that asked for it with that number within the last second. Other networks are not followed.
struct ZbHealth { dect2::ZigbeeHealthFeed feed; bool loaded = false; } H;   // dect2/zigbee/zigbee_health.h
void healthFeed(const dect2::ZigbeeTelemetry& t) {
    if (!H.loaded) { H.loaded = true; H.feed = dect2::ZigbeeHealthFeed((int)plat::prefs().getI("zbMyPan", -1)); }
    if (H.feed.pan() >= 0) H.feed.addLog(t.log);
}
void healthView(App& a) {
    { ImGui::PushTextWrapPos(0);
      ImGui::TextDisabled("%s", tr("Pick your own network (PAN): its nodes get retransmissions, missing acknowledgements, signal trend and silence. Other networks are not followed. Encrypted networks work too: only the MAC header is used."));
      ImGui::PopTextWrapPos(); }
    if (live(a)) for (const auto& p : zt(a).pans) {
        char b[24]; snprintf(b, sizeof b, "%04X", p.pan);
        ImGui::SameLine();
        if (ImGui::RadioButton(b, H.feed.pan() == (int)p.pan)) { H.feed = dect2::ZigbeeHealthFeed(p.pan); H.loaded = true; plat::prefs().setI("zbMyPan", p.pan); plat::prefs().flush(); }
    }
    if (H.feed.pan() < 0) { ImGui::TextDisabled("%s", tr("no network picked")); return; }
    if (!ImGui::BeginTable("##zbh", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    for (const char* c : {"Node", "Frames", "Retries", "No ack", "Level", "Every", "Note"}) ImGui::TableSetupColumn(tr(c));
    ImGui::TableHeadersRow();
    for (const auto& [id, n] : H.feed.health().nodes()) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(id.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)n.frames);
        ImGui::TableNextColumn(); ImGui::Text("%.0f %%", n.retryPct());
        ImGui::TableNextColumn(); if (n.ackAsked) ImGui::Text("%.0f %%", n.missPct()); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (n.rssiNow > -199) ImGui::Text("%.0f (%+.0f)", n.rssiNow, n.rssiNow - n.rssiFirst); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (n.intervalSec > 0) ImGui::Text("%.0f s", n.intervalSec); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); if (!n.text.empty()) ImGui::TextColored(pal::warnAmber(), "%s", n.text.c_str());
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------- hooks

void tick(App& a) {
    loadState();
    if (live(a)) healthFeed(zt(a));
    if (!a.engine.running() || !S.scan || a.devices[a.devIdx].kind == DeviceInfo::File) { S.lastHop = ImGui::GetTime(); return; }
    if (ImGui::GetTime() - S.lastHop < S.dwellSec) return;
    S.lastHop = ImGui::GetTime();
    namespace zb = dect2::zigbee;
    const int cur = zb::channelCodeOf(a.freqMhz);
    int next = zb::kFirstChannel;   // 2.4 GHz: 11 ... 26, then 11 again; 868 / 915 MHz: 868 ch 0, 915 ch 1 ... 10, then 868 ch 0 again (each band scans itself)
    if (zb::isSubGhzChannel(cur)) next = cur >= zb::kSubChannelBase + 10 ? zb::kSubChannelBase : cur + 1;
    else if (cur >= zb::kFirstChannel && cur < zb::kLastChannel) next = cur + 1;
    hopTo(a, next);
}

void tuner(App& a, bool& retune) {
    loadState();
    ImGui::TextDisabled("Channel");
    const int cur = dect2::zigbee::channelCodeOf(a.freqMhz);
    char label[48];
    if (cur) snprintf(label, sizeof label, "%s (%.1f MHz)", dect2::zigbee::channelCodeName(cur).c_str(), dect2::zigbee::channelCodeMhz(cur)); else snprintf(label, sizeof label, "other");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(140 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##zbch", label)) {
        for (int c = dect2::zigbee::kFirstChannel; c <= dect2::zigbee::kLastChannel; c++) {
            char b[48];
            snprintf(b, sizeof b, "%d  %.0f MHz", c, dect2::zigbee::channelMhz(c));
            if (ImGui::Selectable(b, c == cur)) { tuneFreq(a, dect2::zigbee::channelMhz(c)); retune = true; }
        }
        if (!isThread(a)) ImGui::Separator();
        for (int c = dect2::zigbee::kSubChannelBase; !isThread(a) && c <= dect2::zigbee::kSubChannelBase + 10; c++) {   // sub-GHz BPSK: Europe 868.3 MHz, North America / Australia 906 - 924 MHz
            char b[64];
            snprintf(b, sizeof b, "%s  %.1f MHz%s", dect2::zigbee::channelCodeName(c).c_str(), dect2::zigbee::channelCodeMhz(c), c == dect2::zigbee::kSubChannelBase ? "  (Europe, 20 kbit/s)" : "  (US / Australia, 40 kbit/s)");
            if (ImGui::Selectable(b, c == cur)) { tuneFreq(a, dect2::zigbee::channelCodeMhz(c)); retune = true; }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("802.15.4 channels 11 to 26, 2 MHz wide, 5 MHz apart from 2405 MHz. A radio takes in one channel at a time at 4 Msps;\n"
                                                  "channels 15, 20, 25 and 26 are the ones Zigbee networks usually avoid Wi-Fi with. Single-channel mode listens to the one chosen.\n"
                                                  "Below the line: the sub-GHz BPSK channels, 868.3 MHz (Europe) and 906 - 924 MHz in 2 MHz steps (US, Australia), at 20 and 40 kbit/s.");
    ImGui::SameLine(0, 8 * gUi);
    if (ImGui::Checkbox("scan", &S.scan)) { plat::prefs().setB("zbScan", S.scan); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Step through channels 11 to 26, listening to each for the dwell time (a radio that takes in a single 2 MHz channel misses what is sent\n"
                                                  "on the others meanwhile). Off: single channel.");
    if (S.scan) {
        ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(std::min(70 * gUi, ImGui::GetContentRegionAvail().x));
        if (ImGui::SliderInt("##zbdw", &S.dwellSec, 1, 60, "%d s")) { plat::prefs().setI("zbDwell", S.dwellSec); plat::prefs().flush(); }
    }
}

void tab(App& a) {
    S.curThread = isThread(a);
    subNav("zbv", S.view, {"Networks", "Devices", "Frames", tr("Topology"), tr("Health")});
    if (S.view == 0) networksView(a); else if (S.view == 1) devicesView(a); else if (S.view == 3) topoView(a, isThread(a) ? "th" : "zb", zt(a).topo); else if (S.view == 4) healthView(a); else framesView(a);
}

void list(App& a) {
    S.curThread = isThread(a);
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::ZigbeeTelemetry& t = zt(a);
    if (!ImGui::BeginTable("##zbl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Network / device", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Level");
    for (const auto& p : t.pans) {
        ImGui::TableNextRow();
        ImGui::PushID((int)(p.pan * 32 + p.channel));
        const bool recent = t.timeSec - p.lastSec < 3;
        ImGui::TableNextColumn();
        if (ImGui::Selectable("##row", S.selPan == p.pan, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.selPan = S.selPan == p.pan ? -1 : p.pan;
        ImGui::SameLine(0, 0);
        ImGui::PushFont(a.mono, 0); ImGui::TextColored(recent ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "PAN %s", panText(p.pan).c_str()); ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("channel %s, %d devices\n%s", dect2::zigbee::channelCodeName(p.channel).c_str(), p.devices, p.extPan.c_str());
        ImGui::TableNextColumn(); ImGui::TextDisabled("%.0f", p.rssi);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    S.curThread = isThread(a);
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::ZigbeeTelemetry& t = zt(a);
    kvRow(a, "state", "%s", dect2::zigbeeSummary(t).c_str());
    if (t.channel) kvRow(a, "channel", "%s (%.1f MHz)%s", dect2::zigbee::channelCodeName(t.channel).c_str(), dect2::zigbee::channelCodeMhz(t.channel), S.scan ? ", scanning" : "");
    kvRow(a, "signal", "chip SNR %.1f dB, offset %+.1f kHz", t.snrDb, t.cfoHz / 1e3);
    kvRow(a, "frames", "%llu good, %llu failed FCS, %llu preambles, %llu encrypted", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.preambles, (unsigned long long)t.encryptedFrames);
    kvRow(a, "by type", "beacon %llu, data %llu, ack %llu, command %llu", (unsigned long long)t.perType[0], (unsigned long long)t.perType[1], (unsigned long long)t.perType[2], (unsigned long long)t.perType[3]);
    std::string ch;
    for (int i = 0; i < 16; i++) if (t.perChannel[i]) { char b[32]; snprintf(b, sizeof b, "%s%d: %llu", ch.empty() ? "" : ", ", 11 + i, (unsigned long long)t.perChannel[i]); ch += b; }
    kvRow(a, "by channel", "%s", ch.empty() ? "-" : ch.c_str());
    kvRow(a, "input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
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
    lamp("Frames", !on ? 0 : zt(a).state >= 1 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::zigbeeSummary(zt(a)).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = isThread(a) ? "Thread" : "Zigbee / 802.15.4";
    if (live(a)) l2 = dect2::zigbeeSummary(zt(a));
}

} // namespace

extern const ModeUi kZigbeeUi;
const ModeUi kZigbeeUi = {
    .sideTitle = "NETWORKS",
    .tabName = "Zigbee",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .tick = tick,
};

extern const ModeUi kThreadUi;
const ModeUi kThreadUi = {
    .sideTitle = "NETWORKS",
    .tabName = "Thread",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .tick = tick,
};
