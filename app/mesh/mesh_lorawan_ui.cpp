// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mesh (LoRa) screens for LoRaWAN: the device table and the frame log, with their filters and the privacy note.
// Split from mesh_ui.cpp; the shared state is declared in mesh_internal.h.
#include "mesh_internal.h"
#include "ui/ui_common.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

using namespace meshui;

namespace meshui {

// ---------------------------------------------------------------- LoRaWAN

void lorawanPrivacyNote() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Receive only: the frame headers every LoRaWAN gateway in range receives in the clear (message type, DevAddr, frame counter, "
                        "MAC commands, port, MIC; DevEUI and JoinEUI of join requests). The payloads are encrypted with the devices' own keys and "
                        "stay encrypted: OnAir never decrypts them, never checks a MIC and never transmits. These are other people's devices.");
    ImGui::PopTextWrapPos();
}

bool lwMatch(const std::string& hay, const char* needle) {
    if (!needle[0]) return true;
    std::string h = hay, n = needle;
    for (auto& c : h) c = (char)std::tolower((unsigned char)c);
    for (auto& c : n) c = (char)std::tolower((unsigned char)c);
    return h.find(n) != std::string::npos;
}

void lwNetworkCombo(const App& a, const char* id) {
    std::vector<std::string> nets;
    for (const auto& d : a.rx.mesh.lorawanDevices) if (!d.network.empty() && std::find(nets.begin(), nets.end(), d.network) == nets.end()) nets.push_back(d.network);
    std::sort(nets.begin(), nets.end());
    ImGui::SetNextItemWidth(std::min(190 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo(id, S.lwNet.empty() ? "all networks" : S.lwNet.c_str())) {
        if (ImGui::Selectable("all networks", S.lwNet.empty())) S.lwNet.clear();
        for (const auto& n : nets) if (ImGui::Selectable(n.c_str(), S.lwNet == n)) S.lwNet = n;
        ImGui::EndCombo();
    }
}

void lwFilters(const App& a, bool frames) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Filter"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(190 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##lwf", "DevAddr, EUI, network, vendor", S.lwFilter, sizeof S.lwFilter);
    flowNext(10 * gUi);
    lwNetworkCombo(a, "##lwn");
    flowNext(10 * gUi);
    if (frames) {
        static const char* types[] = {"all frames", "uplinks", "downlinks", "joins", "proprietary"};
        ImGui::SetNextItemWidth(std::min(130 * gUi, ImGui::GetContentRegionAvail().x));
        ImGui::Combo("##lwt", &S.lwType, types, 5);
        flowNext(10 * gUi);
        ImGui::Checkbox("hex", &S.lwRaw);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The frame's bytes in hex in the last column (hover a row to see them either way)");
    } else {
        ImGui::Checkbox("join-only devices", &S.lwJoins);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Also list devices heard only by their join requests (by DevEUI): after joining they use a DevAddr\nthe network assigns, which cannot be linked to the DevEUI without the network's keys.");
    }
    flowBreak();
}

bool lwDeviceShown(const dect2::MeshLorawanDevice& d) {
    if (!S.lwJoins && d.devAddr.empty()) return false;
    if (!S.lwNet.empty() && d.network != S.lwNet) return false;
    return lwMatch(d.id + " " + d.devEui + " " + d.joinEui + " " + d.network + " " + d.vendor, S.lwFilter);
}

std::vector<const dect2::MeshLorawanDevice*> sortedDevices(const App& a, bool filtered) {
    std::vector<const dect2::MeshLorawanDevice*> v;
    for (const auto& d : a.rx.mesh.lorawanDevices) if (!filtered || lwDeviceShown(d)) v.push_back(&d);
    std::sort(v.begin(), v.end(), [](const dect2::MeshLorawanDevice* x, const dect2::MeshLorawanDevice* y) { return x->lastHeard > y->lastHeard; });
    return v;
}

std::string lwBattery(const dect2::MeshLorawanDevice& d) {
    if (d.battery < 0) return "";
    char b[48];
    if (d.battery == 0) snprintf(b, sizeof b, "external power, margin %d dB", d.marginDb);
    else if (d.battery == 255) snprintf(b, sizeof b, "battery ?, margin %d dB", d.marginDb);
    else snprintf(b, sizeof b, "battery %.0f %%, margin %d dB", d.battery * 100.0 / 254, d.marginDb);
    return b;
}

void lorawanDevices(const App& a, bool compact) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const bool on = live(a);
    const char* none = !on ? "start the receiver to see devices" : "no LoRaWAN frame heard yet: devices send every few minutes to hours, on 868.1 / 868.3 / 868.5 MHz in Europe";
    if (compact) {
        if (ImGui::BeginTable("##lwd_s", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Device", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Heard"); ImGui::TableSetupColumn("SNR");
            ImGui::TableHeadersRow();
            if (on) for (const auto* d : sortedDevices(a, false)) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextColored(protoColour(5), "%s", d->id.c_str());
                if (ImGui::IsItemHovered() && !d->network.empty()) ImGui::SetTooltip("%s", d->network.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(t.timeSec, d->lastHeard).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.1f", d->lastSnrDb);
            }
            ImGui::EndTable();
        }
        if (!on || t.lorawanDevices.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", none); ImGui::PopTextWrapPos(); }
        return;
    }
    lorawanPrivacyNote();
    lwFilters(a, false);
    const auto devs = sortedDevices(a, true);
    if (on) ImGui::TextDisabled("%zu of %zu devices", devs.size(), t.lorawanDevices.size());
    if (!ImGui::BeginTable("##lwd", 14, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable,
                           ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("DevAddr / DevEUI", ImGuiTableColumnFlags_WidthFixed, hdrW("DevAddr / DevEUI", "DevEUI A84041000181C61A"));
    ImGui::TableSetupColumn("Network", ImGuiTableColumnFlags_WidthFixed, hdrW("Network", "The Things Network"));
    ImGui::TableSetupColumn("Vendor / JoinEUI", ImGuiTableColumnFlags_WidthFixed, hdrW("Vendor / JoinEUI", "A840410000000101"));
    ImGui::TableSetupColumn("Heard"); ImGui::TableSetupColumn("Level dBFS", ImGuiTableColumnFlags_WidthFixed, hdrW("Level dBFS", "-100.0"));
    ImGui::TableSetupColumn("SNR dB", ImGuiTableColumnFlags_WidthFixed, hdrW("SNR dB", "-12.5")); ImGui::TableSetupColumn("FCnt", ImGuiTableColumnFlags_WidthFixed, hdrW("FCnt", "65535"));
    ImGui::TableSetupColumn("Missed", ImGuiTableColumnFlags_WidthFixed, hdrW("Missed", "999")); ImGui::TableSetupColumn("SF"); ImGui::TableSetupColumn("MHz", ImGuiTableColumnFlags_WidthFixed, hdrW("MHz", "868.500"));
    ImGui::TableSetupColumn("Packets", ImGuiTableColumnFlags_WidthFixed, hdrW("Packets", "999 / 99 / 9"));
    ImGui::TableSetupColumn("Last", ImGuiTableColumnFlags_WidthFixed, hdrW("Last", "Unconfirmed down")); ImGui::TableSetupColumn("Port"); ImGui::TableSetupColumn("MAC / status", ImGuiTableColumnFlags_WidthFixed, 260 * gUi);
    ImGui::TableHeadersRow();
    if (on) {
        for (const auto* d : devs) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextColored(protoColour(5), "%s", d->id.c_str()); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(d->network.empty() ? "-" : d->network.c_str());
            ImGui::TableNextColumn();
            if (!d->vendor.empty()) ImGui::TextUnformatted(d->vendor.c_str()); else if (!d->joinEui.empty()) ImGui::TextUnformatted(d->joinEui.c_str()); else ImGui::TextDisabled("-");
            if (ImGui::IsItemHovered() && !d->joinEui.empty()) ImGui::SetTooltip("JoinEUI %s", d->joinEui.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(t.timeSec, d->lastHeard).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.1f", d->lastLevelDb);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", d->lastSnrDb);
            ImGui::TableNextColumn(); if (d->fcnt >= 0) ImGui::Text("%ld", d->fcnt); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (d->missed) ImGui::TextColored(pal::warnAmber(), "%u", d->missed); else ImGui::TextDisabled("0");
            ImGui::TableNextColumn(); ImGui::Text("%d", d->sf);
            ImGui::TableNextColumn(); ImGui::Text("%.3f", d->freqHz / 1e6);
            ImGui::TableNextColumn(); ImGui::Text("%u / %u / %u", d->uplinks, d->downlinks, d->joins);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("uplinks / downlinks / join requests (%u confirmed)", d->confirmed);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(d->lastType.c_str());
            ImGui::TableNextColumn(); if (d->lastFport >= 0) ImGui::Text("%d", d->lastFport); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            const std::string st = lwBattery(*d);
            ImGui::TextUnformatted((d->lastMac + (d->lastMac.empty() || st.empty() ? "" : "; ") + st).c_str());
        }
    }
    ImGui::EndTable();
    if (!on || t.lorawanDevices.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", none); ImGui::PopTextWrapPos(); }
}

void lorawanFrames(const App& a) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const bool on = live(a);
    lwFilters(a, true);
    if (!ImGui::BeginTable("##lwp", 14, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable,
                           ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, hdrW("Type", "Unconfirmed down"));
    ImGui::TableSetupColumn("DevAddr / DevEUI", ImGuiTableColumnFlags_WidthFixed, hdrW("DevAddr / DevEUI", "A84041000181C61A"));
    ImGui::TableSetupColumn("Network", ImGuiTableColumnFlags_WidthFixed, hdrW("Network", "The Things Network"));
    ImGui::TableSetupColumn("FCnt", ImGuiTableColumnFlags_WidthFixed, hdrW("FCnt", "65535")); ImGui::TableSetupColumn("Port");
    ImGui::TableSetupColumn("Payload", ImGuiTableColumnFlags_WidthFixed, hdrW("Payload", "222 B encrypted")); ImGui::TableSetupColumn("FCtrl");
    ImGui::TableSetupColumn("MAC commands", ImGuiTableColumnFlags_WidthFixed, 180 * gUi); ImGui::TableSetupColumn("MIC", ImGuiTableColumnFlags_WidthFixed, hdrW("MIC", "DEADBEEF"));
    ImGui::TableSetupColumn("SF"); ImGui::TableSetupColumn("SNR dB", ImGuiTableColumnFlags_WidthFixed, hdrW("SNR dB", "-12.5"));
    ImGui::TableSetupColumn("MHz", ImGuiTableColumnFlags_WidthFixed, hdrW("MHz", "868.500")); ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthFixed, 380 * gUi);
    ImGui::TableHeadersRow();
    size_t shown = 0;
    if (on) {
        for (size_t i = t.packets.size(); i-- > 0;) {
            const dect2::MeshPacket& p = t.packets[i];
            if (p.protocol != 5) continue;
            const int ty = !p.crcOk || !p.parsed ? -1 : p.mtype == 0 || p.mtype == 1 || p.mtype == 6 ? 3 : p.mtype == 7 ? 4 : (p.mtype == 2 || p.mtype == 4) ? 1 : 2;
            if (S.lwType && ty != S.lwType) continue;
            if (!S.lwNet.empty() && p.network != S.lwNet) continue;
            if (!lwMatch(p.from + " " + p.to + " " + p.network, S.lwFilter)) continue;
            shown++;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(p.timeSec).c_str());
            ImGui::TableNextColumn();
            if (!p.crcOk) ImGui::TextColored(pal::badRed(), "CRC error");
            else if (!p.parsed) ImGui::TextColored(pal::warnAmber(), "not LoRaWAN");
            else ImGui::TextUnformatted(p.type.c_str());
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextColored(protoColour(5), "%s", p.from.c_str()); ImGui::PopFont();
            if (!p.to.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("JoinEUI %s", p.to.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.network.c_str());
            ImGui::TableNextColumn(); if (p.parsed && p.mtype >= 2 && p.mtype <= 5) ImGui::Text("%u", p.packetId); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (p.fport >= 0) ImGui::Text("%d", p.fport); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (p.frmLen >= 0) ImGui::TextColored(pal::warnAmber(), "%d B encrypted", p.frmLen); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.flags.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.mac.c_str());
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); if (p.parsed && p.mtype != 7) ImGui::Text("%08X", p.mic); else ImGui::TextDisabled("-"); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::Text("%d", p.sf);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", p.snrDb);
            ImGui::TableNextColumn(); ImGui::Text("%.3f", p.freqHz / 1e6);
            ImGui::TableNextColumn();
            if (S.lwRaw && !p.raw.empty()) { ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(p.raw.c_str()); ImGui::PopFont(); }
            else ImGui::TextUnformatted(p.parsed ? p.detail.c_str() : p.note.c_str());
            if (!p.raw.empty() && ImGui::IsItemHovered()) { wrapTooltip(560 * gUi, p.raw.c_str()); }
        }
    }
    ImGui::EndTable();
    if (!on || !shown) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", !on ? "start the receiver to see frames" : "no LoRaWAN frame (that the filters let through) yet"); ImGui::PopTextWrapPos(); }
}

}  // namespace meshui
