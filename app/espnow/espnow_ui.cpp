// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ESP-NOW screens: the Devices table (sender address, Espressif or not, packets, packet rate, level, PHY rate, last payload), the Messages log
// with the selected packet as hex and text, and the channel picker (Wi-Fi channels 1 - 14, or auto: hop over them with a dwell time).
// Receive only: nothing is transmitted. Encrypted packets are shown as encrypted; only a key the user entered (Tools > Keys, ESP-NOW) opens them.
#include "app.h"
#include "../wifi/wifi_ui_int.h"
#include "dect2/common/time_fmt.h"
#include "dect2/espnow/espnow_proto.h"
#include "dect2/espnow/espnow_rx.h"
#include "dect2/espnow/espnow_tel.h"
#include "dect2/wifi/wifi_proto.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;                        // 0 devices, 1 messages
    bool loaded = false;
    int chan = 1;                        // 0 auto (hop), 1 - 14
    int dwellMs = 300;
    bool with14 = false;                 // auto: channel 14 too (Japan, DSSS only)
    bool badFcs = false;
    double lastHop = 0;
    std::string sel;                     // selected device
    int selMsg = -1;                     // selected message: its index in the log when picked
    double selSec = -1; std::string selFrom;   // and what it was, to follow it as the log grows
    char filter[64] = "";
};
State S;

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.chan = std::max(0, std::min(14, (int)plat::prefs().getI("espnowChan", 1)));
    S.dwellMs = std::max(100, std::min(5000, (int)plat::prefs().getI("espnowDwellMs", 300)));
    S.with14 = plat::prefs().getB("espnowCh14", false);
    S.badFcs = plat::prefs().getB("espnowBadFcs", false);
}
void save() {
    plat::prefs().setI("espnowChan", S.chan); plat::prefs().setI("espnowDwellMs", S.dwellMs);
    plat::prefs().setB("espnowCh14", S.with14); plat::prefs().setB("espnowBadFcs", S.badFcs);
}

bool live(const App& a) { return a.rx.standard == 899; }   // the engine reports its standard code minus one

std::string clock(int64_t t) { return t <= 0 ? "-" : dect2::timefmt::clock(t); }

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

std::string rateText(int kbps, bool shortPre) {
    char b[48];
    if (kbps <= 0) return "-";
    if (kbps >= 6000) snprintf(b, sizeof b, "OFDM %d", kbps / 1000);
    else snprintf(b, sizeof b, "DSSS %d%s", kbps / 1000, shortPre ? " short" : "");
    return b;
}

std::string payloadLine(const std::vector<uint8_t>& p, size_t max = 24) {
    if (p.empty()) return "";
    if (dect2::espnow::mostlyText(p.data(), p.size())) return "\"" + dect2::espnow::printable(p.data(), p.size(), 48) + "\"";
    return dect2::espnow::hexText(p.data(), p.size(), max);
}

void receiveNote() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Receive only: OnAir never transmits. Needs a radio with 20 MHz of bandwidth on 2.4 GHz (HackRF, Pluto, LimeSDR, USRP, bladeRF). "
                                 "Encrypted packets are shown as encrypted; a key you enter under Tools > Keys (protocol ESP-NOW) opens your own."));
    ImGui::PopTextWrapPos();
}

// ---------------------------------------------------------------- channel and hop

std::vector<int> hopList() {
    std::vector<int> v;
    for (int c = 1; c <= (S.with14 ? 14 : 13); c++) v.push_back(c);
    return v;
}
void tuneChannel(App& a, int ch) {
    const double mhz = dect2::wifi::channelMhz(ch);
    if (mhz <= 0) return;
    a.freqMhz = mhz;
    a.tune.centerHz = mhz * 1e6;
    a.engine.retune(a.tune);
}
bool isFile(const App& a) { return a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[(size_t)a.devIdx].kind == DeviceInfo::File; }

void hopTick(App& a) {
    if (S.chan != 0 || !a.engine.running() || isFile(a)) { S.lastHop = ImGui::GetTime(); return; }
    if (ImGui::GetTime() - S.lastHop < S.dwellMs / 1000.0) return;
    S.lastHop = ImGui::GetTime();
    const std::vector<int> l = hopList();
    const int cur = dect2::wifi::channelOf(a.freqMhz);
    size_t next = 0;
    for (size_t i = 0; i < l.size(); i++) if (l[i] == cur) next = (i + 1) % l.size();
    tuneChannel(a, l[next]);
}

void tuner(App& a, bool& retune) {
    loadState();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Channel")); ImGui::SameLine(0, 5 * gUi);
    const int cur = dect2::wifi::channelOf(a.freqMhz);
    char label[48];
    if (S.chan == 0) snprintf(label, sizeof label, "%s (%d)", tr("Auto, hopping"), cur);
    else snprintf(label, sizeof label, "%d (%.0f MHz)", S.chan, dect2::wifi::channelMhz(S.chan));
    ImGui::SetNextItemWidth(std::min(170 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##espch", label)) {
        if (ImGui::Selectable(tr("Auto, hopping"), S.chan == 0)) { S.chan = 0; save(); S.lastHop = 0; }
        for (int c = 1; c <= 14; c++) {
            char b[40]; snprintf(b, sizeof b, "%d (%.0f MHz)", c, dect2::wifi::channelMhz(c));
            if (ImGui::Selectable(b, S.chan == c)) { S.chan = c; save(); tuneFreq(a, dect2::wifi::channelMhz(c)); retune = true; }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("An ESP32 that is not joined to an access point sends ESP-NOW on channel 1 unless its program sets another; one joined to a network uses the network's channel. Auto steps through the channels."));
    if (S.chan == 0) {
        flowNext(10 * gUi);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", tr("Dwell")); ImGui::SameLine(0, 5 * gUi);
        ImGui::SetNextItemWidth(std::min(140 * gUi, ImGui::GetContentRegionAvail().x));
        if (ImGui::SliderInt("##espdw", &S.dwellMs, 100, 5000, "%d ms")) save();
        flowNext(10 * gUi);
        if (ImGui::Checkbox(tr("Channel 14"), &S.with14)) save();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Japan only, 2484 MHz"));
    }
    flowBreak();
    const std::string note = wfui::radioNote(a);
    if (!note.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::warnAmber(), "%s", note.c_str()); ImGui::PopTextWrapPos(); }
}

void decoder(App& a, bool& /*retune*/) {
    loadState();
    if (ImGui::Checkbox(tr("List frames with a bad FCS"), &S.badFcs)) save();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("ESP-NOW frames whose checksum failed are listed in Messages, marked; they never count as a device"));
    a.engine.espnow().setShowBadFcs(S.badFcs);
}

// ---------------------------------------------------------------- views

bool passes(const dect2::EspnowDevice& d) {
    if (!S.filter[0]) return true;
    const std::string f = lower(S.filter);
    return lower(d.mac).find(f) != std::string::npos || lower(d.vendor).find(f) != std::string::npos ||
           lower(dect2::espnow::printable(d.lastPayload.data(), d.lastPayload.size())).find(f) != std::string::npos;
}

void filters(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Filter")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(180 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##espf", "address, vendor, payload", S.filter, sizeof S.filter);
    flowNext(10 * gUi);
    if (ImGui::Button(tr("Clear"))) { a.engine.espnow().clearDevices(); S.sel.clear(); S.selMsg = -1; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Forget every device and message heard so far"));
    flowBreak();
}

void devicesView(App& a) {
    receiveNote();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to see the ESP-NOW devices around you")); return; }
    filters(a);
    const dect2::EspnowTelemetry& t = a.rx.espnow;
    if (!ImGui::BeginTable("##espd", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Sender"); ImGui::TableSetupColumn("Vendor"); ImGui::TableSetupColumn("Packets"); ImGui::TableSetupColumn("Rate");
    ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("PHY"); ImGui::TableSetupColumn("To"); ImGui::TableSetupColumn("Last seen");
    ImGui::TableSetupColumn("Last payload", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& d : t.devices) {
        if (!passes(d)) continue;
        ImGui::TableNextRow();
        ImGui::PushID(d.mac.c_str());
        const bool recent = t.timeSec - d.lastSec < 3;
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, recent ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text));
        ImGui::PushFont(a.mono, 0);
        if (ImGui::Selectable(d.mac.c_str(), S.sel == d.mac, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.sel = d.mac;
        ImGui::PopFont();
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\nESP-NOW v%d, %llu packets (%llu broadcast, %llu encrypted)\nchannel %d, first heard %s%s", d.mac.c_str(), d.version,
                              (unsigned long long)d.packets, (unsigned long long)d.broadcasts, (unsigned long long)d.encrypted, d.channel,
                              clock(d.firstWall).c_str(), dect2::timefmt::sfx());
        ImGui::TableNextColumn();
        if (d.espressif) ImGui::TextColored(pal::okGreen(), "%s", "Espressif"); else ImGui::TextDisabled("%s", d.vendor.empty() ? "-" : d.vendor.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)d.packets);
        ImGui::TableNextColumn(); if (d.rate > 0) ImGui::Text("%.1f/s", d.rate); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::Text("%.0f", d.rssi);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(rateText(d.rateKbps, false).c_str());
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(d.lastTo.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(d.lastWall).c_str());
        ImGui::TableNextColumn();
        if (d.lastEncrypted) ImGui::TextColored(pal::warnAmber(), "%s", tr("encrypted"));
        else { ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(payloadLine(d.lastPayload).c_str()); ImGui::PopFont(); }
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (t.devices.empty()) ImGui::TextDisabled("%s", tr("no ESP-NOW devices yet"));
}

void hexDump(const App& a, const std::vector<uint8_t>& p) {
    ImGui::PushFont(a.mono, 0);
    for (size_t i = 0; i < p.size(); i += 16) {
        char b[100];
        int k = snprintf(b, sizeof b, "%04zX  ", i);
        for (size_t j = 0; j < 16; j++) {
            if (j < p.size() - i) k += snprintf(b + k, sizeof b - (size_t)k, "%02X ", p[i + j]);
            else k += snprintf(b + k, sizeof b - (size_t)k, "%s", "   ");
        }
        std::string line = b;
        line += ' ';
        line += dect2::espnow::printable(p.data() + i, std::min<size_t>(16, p.size() - i));
        ImGui::TextUnformatted(line.c_str());
    }
    ImGui::PopFont();
}

void messagesView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    filters(a);
    const auto& log = a.rx.espnow.log;
    // follow the selected message as new ones push it down
    if (S.selMsg >= 0) {
        S.selMsg = -1;
        for (size_t i = 0; i < log.size(); i++) if (log[i].sec == S.selSec && log[i].from == S.selFrom) { S.selMsg = (int)i; break; }
    }
    const float detailH = S.selMsg >= 0 ? std::min(ImGui::GetContentRegionAvail().y * 0.45f, 260 * gUi) : 0.f;
    if (ImGui::BeginTable("##espm", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                          ImVec2(0, ImGui::GetContentRegionAvail().y - detailH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Ch"); ImGui::TableSetupColumn("PHY"); ImGui::TableSetupColumn("From");
        ImGui::TableSetupColumn("To"); ImGui::TableSetupColumn("Seq"); ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)log.size());
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                const auto& m = log[(size_t)i];
                ImGui::TableNextRow();
                ImGui::PushID(i);
                ImGui::TableNextColumn();
                if (ImGui::Selectable(clock(m.wallTime).c_str(), S.selMsg == i, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                    S.selMsg = i; S.selSec = m.sec; S.selFrom = m.from;
                }
                ImGui::TableNextColumn(); if (m.channel) ImGui::Text("%d", m.channel); else ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(rateText(m.rateKbps, m.shortPreamble).c_str());
                ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(m.from.c_str()); ImGui::PopFont();
                ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(m.to.c_str()); ImGui::PopFont();
                ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned)m.seq);
                ImGui::TableNextColumn(); ImGui::Text("%.0f", m.rssi);
                ImGui::TableNextColumn();
                if (!m.fcsOk) ImGui::TextColored(pal::warnAmber(), "%s", m.info.c_str());
                else if (m.encrypted && !m.decrypted) ImGui::TextColored(pal::warnAmber(), "%s", m.info.c_str());
                else ImGui::TextUnformatted(m.info.c_str());
                ImGui::PopID();
            }
        ImGui::EndTable();
    }
    if (log.empty()) ImGui::TextDisabled("%s", tr("no messages yet"));
    if (S.selMsg >= 0 && S.selMsg < (int)log.size()) {
        const auto& m = log[(size_t)S.selMsg];
        ImGui::BeginChild("##espdet", ImVec2(0, 0), ImGuiChildFlags_Borders);
        kvRow(a, "frame", "%s to %s, sequence %u, ESP-NOW v%d, %s, FCS %s", m.from.c_str(), m.to.c_str(), (unsigned)m.seq, m.version,
              rateText(m.rateKbps, m.shortPreamble).c_str(), m.fcsOk ? "good" : "bad");
        kvRow(a, "signal", "%.1f dBFS, SNR %.1f dB, offset %+.1f kHz", m.rssi, m.snrDb, m.cfoHz / 1e3);
        if (m.encrypted && !m.decrypted) kvRow(a, "payload", "encrypted (CCMP): enter the peer's LMK under Tools > Keys, protocol ESP-NOW, to read your own");
        else if (m.payload.empty()) kvRow(a, "payload", "empty");
        else {
            kvRow(a, "payload", "%zu bytes%s", m.payload.size(), m.decrypted ? ", decrypted with your key" : "");
            if (dect2::espnow::mostlyText(m.payload.data(), m.payload.size())) kvRow(a, "text", "%s", dect2::espnow::printable(m.payload.data(), m.payload.size()).c_str());
            hexDump(a, m.payload);
        }
        ImGui::EndChild();
    }
}

// ---------------------------------------------------------------- hooks

void tab(App& a) {
    loadState();
    subNav("espv", S.view, {"Devices", "Messages"});
    if (S.view == 0) devicesView(a);
    else messagesView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::EspnowTelemetry& t = a.rx.espnow;
    if (!ImGui::BeginTable("##espl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Pkts");
    for (const auto& d : t.devices) {
        ImGui::TableNextRow();
        ImGui::PushID(d.mac.c_str());
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, t.timeSec - d.lastSec < 3 ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text));
        if (ImGui::Selectable(d.mac.c_str(), S.sel == d.mac)) { S.sel = d.mac; S.view = 0; }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", d.vendor.empty() ? "vendor not known" : d.vendor.c_str(),
                                                      d.lastEncrypted ? "encrypted" : payloadLine(d.lastPayload, 16).c_str());
        ImGui::TableNextColumn(); ImGui::TextDisabled("%llu", (unsigned long long)d.packets);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::EspnowTelemetry& t = a.rx.espnow;
    kvRow(a, "state", "%s", dect2::espnowSummary(t).c_str());
    if (t.channel) kvRow(a, "channel", "%d (%.0f MHz, %+.1f MHz from the centre)%s", t.channel, dect2::wifi::channelMhz(t.channel), t.offsetHz / 1e6, S.chan == 0 ? ", hopping" : "");
    else kvRow(a, "channel", "%s", t.centerMhz > 0 ? "no Wi-Fi channel within 12 MHz of the centre" : "at 0 Hz (a file)");
    kvRow(a, "frames", "%llu good 802.11 frames, %llu action frames, %llu ESP-NOW; %llu failed the FCS", (unsigned long long)t.wifiFrames,
          (unsigned long long)t.actionFrames, (unsigned long long)t.espnowFrames, (unsigned long long)t.blocksBad);
    kvRow(a, "DSSS", "%llu SFDs found, %llu headers good, %llu bad, %llu at CCK rates (not read)", (unsigned long long)t.sfdHits, (unsigned long long)t.headersOk,
          (unsigned long long)t.headersBad, (unsigned long long)t.otherRate);
    kvRow(a, "ESP-NOW", "%llu broadcast, %llu encrypted, %llu decrypted with your keys", (unsigned long long)t.broadcastFrames,
          (unsigned long long)t.encryptedFrames, (unsigned long long)t.decryptedFrames);
    kvRow(a, "PHY", "DSSS 1 Mbit/s %llu, 2 Mbit/s %llu (short preamble %llu), OFDM 6 Mbit/s %llu, 12 Mbit/s %llu", (unsigned long long)t.perRate[0],
          (unsigned long long)t.perRate[1], (unsigned long long)t.perRate[2], (unsigned long long)t.perRate[3], (unsigned long long)t.perRate[4]);
    std::string per;
    for (int c = 1; c <= 14; c++) if (t.perChannel[c]) { char b[32]; snprintf(b, sizeof b, "%s%d: %llu", per.empty() ? "" : ", ", c, (unsigned long long)t.perChannel[c]); per += b; }
    if (!per.empty()) kvRow(a, "per channel", "%s", per.c_str());
    kvRow(a, "signal", "SNR %.1f dB, offset %+.1f kHz", t.snrDb, t.cfoHz / 1e3);
    kvRow(a, "input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    ImGui::Spacing();
    receiveNote();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("ESP-NOW", !on ? 0 : a.rx.espnow.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::espnowSummary(a.rx.espnow).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "ESP-NOW";
    if (live(a)) l2 = dect2::espnowSummary(a.rx.espnow);
}

void tick(App& a) {
    loadState();
    if (!live(a)) return;
    a.engine.espnow().setShowBadFcs(S.badFcs);
    hopTick(a);
}

} // namespace

extern const ModeUi kEspnowUi;
const ModeUi kEspnowUi = {
    .sideTitle = "DEVICES",
    .tabName = "ESP-NOW",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .decoder = decoder,
    .tick = tick,
};
