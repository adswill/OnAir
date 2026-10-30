// nRF24L01 screens: the Enhanced ShockBurst frames heard (address, control field, payload as hex and text), the addresses (devices) and the bursts
// that gave no frame. Receive only. A 2.4 GHz SDR is needed: HackRF (One, Pro), PlutoSDR (extended range), bladeRF, LimeSDR, USRP B200/B210/B205mini.
#include "app.h"
#include "i18n/i18n.h"
#include "dect2/common/time_fmt.h"
#include "dect2/nrf24/nrf24_rx.h"
#include "dect2/nrf24/nrf24_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct State { int view = 0; };
State S;

bool live(const App& a) { return a.rx.standard == 210; }   // the engine reports its standard code minus one

void banner(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s", tr("Needs a 2.4 GHz SDR"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Radios that reach 2.4 GHz and work here: HackRF One / Pro, PlutoSDR (with the extended frequency range), bladeRF, LimeSDR, USRP B200 / B210 / B205mini. Not possible: RTL-SDR (up to 1.7 GHz), Airspy R2 / Mini, SDRplay RSP1A / RSPduo / RSPdx and sound cards."));
    flowNext(8 * gUi);
    ImGui::TextDisabled("%s", tr("Tune to a channel: 2400 + N MHz (N = 0..125). 250 kbit/s, 1 and 2 Mbit/s are tried together."));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Enhanced ShockBurst frames of nRF24L01(+) chips (wireless mice and keyboards, sensors, toys, RC links). The address is not on the air as such: it is found by trying 3 to 5 byte addresses and accepting a frame whose CRC is valid. A frame with a CRC-8 (less certain than a CRC-16 one) is shown once it is seen three times. Frames of other 2.4 GHz systems (Bluetooth LE, Wi-Fi, Zigbee) are not ESB and give no frame."));
    flowNext(10 * gUi);
    char cl[40]; snprintf(cl, sizeof cl, "%s##nrf", tr("Clear"));
    if (ImGui::Button(cl)) a.engine.nrf24().clearResults();
    flowEnd();
}

int channelOf(const App& a) { return (int)std::lround(a.freqMhz - 2400.0); }

void framesView(App& a) {
    const auto& fr = a.rx.nrf24.frames;
    if (!ImGui::BeginTable("##nrff", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn(tr("Rate")); ImGui::TableSetupColumn(tr("Address")); ImGui::TableSetupColumn("PID"); ImGui::TableSetupColumn("CRC");
    ImGui::TableSetupColumn(tr("Length")); ImGui::TableSetupColumn(tr("Copies")); ImGui::TableSetupColumn(tr("Payload (hex)"), ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn(tr("Text"));
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    for (const auto& f : fr) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(f.utc).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%d k", f.rateKbps);
        ImGui::TableNextColumn(); ImGui::Text("%s", f.address.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%d%s", f.pid, f.noAck ? " NA" : "");
        ImGui::TableNextColumn(); ImGui::Text("%d", f.crcBits);
        ImGui::TableNextColumn(); ImGui::Text("%d", f.payloadLen);
        ImGui::TableNextColumn(); ImGui::Text("%d", f.repeats);
        ImGui::TableNextColumn(); ImGui::PushTextWrapPos(0); ImGui::Text("%s", f.payloadHex.c_str()); ImGui::PopTextWrapPos();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", f.text.c_str());
    }
    ImGui::PopFont();
    ImGui::EndTable();
    if (fr.empty()) ImGui::TextDisabled("%s", tr("no frames yet"));
}

void devicesView(App& a) {
    const auto& dv = a.rx.nrf24.devices;
    if (!ImGui::BeginTable("##nrfd", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Address")); ImGui::TableSetupColumn(tr("Bytes")); ImGui::TableSetupColumn(tr("Rate")); ImGui::TableSetupColumn("CRC");
    ImGui::TableSetupColumn(tr("Last heard")); ImGui::TableSetupColumn(tr("Frames"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    for (const auto& d : dv) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%s", d.address.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%d", d.addrLen);
        ImGui::TableNextColumn(); ImGui::Text("%d k", d.rateKbps);
        ImGui::TableNextColumn(); ImGui::Text("%d", d.crcBits);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(d.lastSeen).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)d.count);
    }
    ImGui::PopFont();
    ImGui::EndTable();
    if (dv.empty()) ImGui::TextDisabled("%s", tr("no addresses yet"));
}

void burstsView(App& a) {
    const auto& bs = a.rx.nrf24.burstLog;
    if (!ImGui::BeginTable("##nrfu", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn(tr("Duration")); ImGui::TableSetupColumn("dB", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    for (const auto& b : bs) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(b.utc).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.0f us", b.durationUs);
        ImGui::TableNextColumn(); ImGui::Text("%.0f", b.snrDb);
    }
    ImGui::PopFont();
    ImGui::EndTable();
    if (bs.empty()) ImGui::TextDisabled("%s", tr("no other bursts"));
}

void tab(App& a) {
    banner(a);
    ImGui::Spacing();
    subNav("nrfv", S.view, {"Frames", "Addresses", "Other bursts"});
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    if (S.view == 0) framesView(a); else if (S.view == 1) devicesView(a); else burstsView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const auto& dv = a.rx.nrf24.devices;
    if (!ImGui::BeginTable("##nrfl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn(tr("Address")); ImGui::TableSetupColumn(tr("Heard"), ImGuiTableColumnFlags_WidthStretch);
    for (const auto& d : dv) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::Text("%s", d.address.c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s, %llux", dect2::timefmt::clock(d.lastSeen).c_str(), (unsigned long long)d.count);
    }
    ImGui::EndTable();
    if (dv.empty()) ImGui::TextDisabled("%s", tr("no addresses yet"));
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::Nrf24Telemetry& t = a.rx.nrf24;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv(tr("state"), "%s", dect2::nrf24Summary(t).c_str());
    kv(tr("channel"), "%d (%.0f MHz)", channelOf(a), 2400.0 + channelOf(a));
    kv(tr("frames"), "%llu (CRC-16 %llu, CRC-8 %llu)", (unsigned long long)t.blocksOk, (unsigned long long)t.crc16Frames, (unsigned long long)t.crc8Frames);
    kv(tr("by rate"), "250k %llu, 1M %llu, 2M %llu", (unsigned long long)t.frames250, (unsigned long long)t.frames1M, (unsigned long long)t.frames2M);
    kv(tr("bursts"), "%llu (%llu %s)", (unsigned long long)t.bursts, (unsigned long long)t.blocksBad, tr("without a frame"));
    kv(tr("carrier offset"), "%+.0f kHz", t.cfoHz / 1e3);
    kv(tr("input"), "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kv(tr("signal time"), "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Burst"), !on ? 0 : a.rx.nrf24.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp(tr("Frame"), !on ? 0 : a.rx.nrf24.dataValid ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::nrf24Summary(a.rx.nrf24).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "nRF24L01";
    if (live(a)) l2 = dect2::nrf24Summary(a.rx.nrf24);
}

} // namespace

extern const ModeUi kNrf24Ui;
const ModeUi kNrf24Ui = {
    .sideTitle = "ADDRESSES",
    .tabName = "nRF24",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = nullptr,
};
