// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DGPS beacon screens: the station ID, bit rate, carrier offset, level, message counts, the decoded messages (corrections, reference station position, almanac, text).
#include "app.h"
#include "dect2/dgps/dgps_engine.h"
#include <cstdio>
#include <string>

namespace {

bool live(const App& a) { return a.rx.standard == 121; }   // family 120, engine standard 122, active standard 121

std::string typesText(const dect2::DgpsTelemetry& t) {
    if (t.typesSeen.empty()) return "-";
    std::string s;
    for (int ty : t.typesSeen) {
        if (!s.empty()) s += ", ";
        s += std::to_string(ty);
    }
    return s;
}

void tab(App& a) {
    if (!live(a)) {
        ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "tune to about 300 kHz (283.5 - 325 kHz) and start the receiver");
        return;
    }
    const dect2::DgpsTelemetry& t = a.rx.dgps;
    ImGui::Text("DGPS beacon: MSK at %.0f bit/s, RTCM SC-104 messages", t.dataValid ? t.bitRate : 200.0);
    ImGui::Spacing();
    if (t.dataValid) ImGui::Text("Station ID: %d", t.stationId);
    else ImGui::TextDisabled("Station ID: -");
    ImGui::Text("Carrier offset: %+.1f Hz", t.cfoHz);
    ImGui::Text("Level: %.0f dBFS", t.levelDb);
    ImGui::Text("Frames: %llu, complete messages: %llu, words with bad parity: %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.messagesComplete, (unsigned long long)t.wordsBad);
    ImGui::Text("Message types seen: %s", typesText(t).c_str());
    if (t.dataValid) ImGui::Text("Last frame: type %d, Z-count %d (0.6 s units), health %d", t.lastType, t.zcount, t.health);
    if (t.hasPosition) ImGui::Text("Reference station: %.5f, %.5f, %.1f m above the ellipsoid", t.lat, t.lon, t.height);
    if (t.satellites) ImGui::Text("Satellites corrected: %d", t.satellites);
    if (!t.specialText.empty()) ImGui::Text("Station message: %s", t.specialText.c_str());
    if (!t.almanac.empty() && ImGui::BeginTable("dgalm", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Beacon"); ImGui::TableSetupColumn("Frequency"); ImGui::TableSetupColumn("Position"); ImGui::TableSetupColumn("Range"); ImGui::TableSetupColumn("Bit rate"); ImGui::TableSetupColumn("Health");
        ImGui::TableHeadersRow();
        for (const dect2::DgpsBeacon& b : t.almanac) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d", b.stationId);
            ImGui::TableNextColumn(); ImGui::Text("%.1f kHz", b.frequencyKhz);
            ImGui::TableNextColumn(); ImGui::Text("%.3f, %.3f", b.latitude, b.longitude);
            ImGui::TableNextColumn(); ImGui::Text("%d km", b.rangeKm);
            ImGui::TableNextColumn(); ImGui::Text("%d bit/s", b.bitRate);
            ImGui::TableNextColumn(); ImGui::Text("%d", b.health);
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Input %.0f Hz, centre %.4f MHz", t.inputRate, t.centerHz / 1e6);
    if (!t.dataValid) ImGui::TextDisabled("listening: no frame with a good header yet");
}

void receiver(App& a) {
    if (!live(a)) return;
    const dect2::DgpsTelemetry& t = a.rx.dgps;
    ImGui::Text("%s", dect2::dgpsSummary(t).c_str());
    ImGui::Text("frames %llu, level %.1f dBFS, offset %+.1f Hz", (unsigned long long)t.blocksOk, t.levelDb, t.cfoHz);
}

void list(App& a) {
    if (!live(a)) return;
    const dect2::DgpsTelemetry& t = a.rx.dgps;
    if (ImGui::BeginTable("dgmsg", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("Station"); ImGui::TableSetupColumn("Z-count"); ImGui::TableSetupColumn("Health"); ImGui::TableSetupColumn("Content", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const dect2::DgpsMessageRow& r : t.rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d", r.type);
            ImGui::TableNextColumn(); ImGui::Text("%d", r.station);
            ImGui::TableNextColumn(); ImGui::Text("%d", r.zcount);
            ImGui::TableNextColumn(); ImGui::Text("%d", r.health);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.text.c_str());
        }
        ImGui::EndTable();
    }
}
void tick(App&) {}

void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State");
    ImGui::SameLine();
    ImGui::TextUnformatted(live(a) ? dect2::dgpsSummary(a.rx.dgps).c_str() : (a.engine.running() ? "starting" : "stopped"));
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "DGPS beacon";
    if (live(a)) l2 = dect2::dgpsSummary(a.rx.dgps);
}

} // namespace

extern const ModeUi kDgpsUi;
const ModeUi kDgpsUi = {
    .sideTitle = "MESSAGES",
    .tabName = "DGPS",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
