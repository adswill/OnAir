// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Drone Remote ID screens: the drone table (serial number or session ID, UA type, position, altitude and height, speed and track, operator
// position, last seen), a map with each drone, its trail and a line to its operator, the message log, and the band picker.
// Receive only: Remote ID (ASTM F3411 / ASD-STAN EN 4709-002) is a public broadcast meant for anyone to read; nothing is transmitted.
#include "map/my_location.h"
#include "dect2/rid/rid_alerts.h"
#include "dect2/airports/airports.h"
#include <deque>
#include <unordered_map>
#include "dect2/common/time_fmt.h"
#include "../adsb/adsb_map.h"
#include "app.h"
#include "dect2/ble/ble_proto.h"
#include "dect2/rid/rid_proto.h"
#include "dect2/rid/rid_rx.h"
#include "dect2/rid/rid_tel.h"
#include "dect2/wifi/wifi_proto.h"
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
    int view = 0;                        // 0 drones, 1 map, 2 messages
    bool loaded = false, mapFollow = true;
    adsbmap::View map;
    std::string sel;                     // the selected drone's address
    char filter[64] = "";
};
State S;

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.map.zoom = (int)plat::prefs().getI("ridZoom", 14);
}

bool live(const App& a) { return a.rx.standard == 57; }   // the engine reports its standard code minus one

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

std::string idOf(const dect2::RidDrone& d) {
    if (!d.serial.empty()) return d.serial;
    if (!d.sessionId.empty()) return d.sessionId;
    if (!d.caaId.empty()) return d.caaId;
    if (!d.utmId.empty()) return d.utmId;
    return d.mac;
}

bool passes(const dect2::RidDrone& d) {
    if (!S.filter[0]) return true;
    const std::string f = lower(S.filter);
    for (const std::string* s : {&d.mac, &d.serial, &d.sessionId, &d.caaId, &d.utmId, &d.operatorId, &d.selfId})
        if (lower(*s).find(f) != std::string::npos) return true;
    return false;
}

std::string altText(float m) { if (m <= -1000) return "-"; char b[24]; snprintf(b, sizeof b, "%.0f m", m); return b; }

void receiveNote() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Receive only: Remote ID is the identification drones must broadcast openly (EU 2019/945, FAA part 89), meant for anyone to read. "
                        "OnAir never transmits. Heard: Bluetooth LE legacy advertising (BLE 4) and Wi-Fi beacons; BLE 5 long range and Wi-Fi NAN are not.");
    ImGui::PopTextWrapPos();
}

void filters(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Filter"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(180 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##ridf", "serial, session, operator, address", S.filter, sizeof S.filter);
    flowNext(10 * gUi);
    if (ImGui::Button("Clear")) { a.engine.rid().clearDrones(); S.sel.clear(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forget every drone and message heard so far");
    flowBreak();
}

void dronesView(App& a) {
    receiveNote();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the drones around you"); return; }
    filters(a);
    const dect2::RidTelemetry& t = a.rx.rid;
    if (!ImGui::BeginTable("##ridt", 11, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("ID"); ImGui::TableSetupColumn("UA type"); ImGui::TableSetupColumn("Status"); ImGui::TableSetupColumn("Position");
    ImGui::TableSetupColumn("Alt"); ImGui::TableSetupColumn("Height"); ImGui::TableSetupColumn("Speed / track"); ImGui::TableSetupColumn("Operator");
    ImGui::TableSetupColumn("Via"); ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("Last seen", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    int shown = 0;
    for (const auto& d : t.drones) {
        if (!passes(d)) continue;
        shown++;
        ImGui::TableNextRow();
        ImGui::PushID(d.mac.c_str());
        const double ago = std::max(0.0, t.timeSec - d.lastSec);
        const ImVec4 c = ago < 3 ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, c);
        if (ImGui::Selectable(idOf(d).c_str(), S.sel == d.mac, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.sel = d.mac;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("address %s\nserial %s\nsession ID %s\nCAA registration %s\noperator ID %s\nSelf-ID %s\nEU %s, class %s\n%llu messages, first heard %s%s%s",
                              d.mac.c_str(), d.serial.empty() ? "-" : d.serial.c_str(), d.sessionId.empty() ? "-" : d.sessionId.c_str(), d.caaId.empty() ? "-" : d.caaId.c_str(),
                              d.operatorId.empty() ? "-" : d.operatorId.c_str(), d.selfId.empty() ? "-" : d.selfId.c_str(), dect2::rid::categoryName(d.category),
                              dect2::rid::className(d.uaClass).c_str(), (unsigned long long)d.messages, clock(d.firstWall).c_str(), dect2::timefmt::sfx(),
                              d.authType > 0 ? (std::string("\nauthentication: ") + dect2::rid::authTypeName(d.authType)).c_str() : "");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(d.uaType < 0 ? "-" : dect2::rid::uaTypeName(d.uaType));
        ImGui::TableNextColumn();
        if (d.status == 3 || d.status == 4) ImGui::TextColored(pal::warnAmber(), "%s", dect2::rid::statusName(d.status)); else ImGui::TextUnformatted(dect2::rid::statusName(d.status));
        ImGui::TableNextColumn();
        if (d.hasPos) { ImGui::PushFont(a.mono, 0); ImGui::Text("%.6f %.6f", d.lat, d.lon); ImGui::PopFont(); } else ImGui::TextDisabled("-");
        if (d.hasPos && ImGui::IsItemHovered()) ImGui::SetTooltip("accuracy: horizontal %s, vertical %s", dect2::rid::horizAccText(d.horizAcc).c_str(), dect2::rid::vertAccText(d.vertAcc).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(altText(d.altGeo).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("geodetic (WGS 84) altitude; pressure altitude %s", altText(d.altBaro).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(altText(d.height).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(d.heightType ? "height above the ground" : "height above the take-off point");
        ImGui::TableNextColumn();
        if (d.speed >= 0) {
            char b[64];
            if (d.direction >= 0) snprintf(b, sizeof b, "%.1f m/s %03.0f", d.speed, d.direction); else snprintf(b, sizeof b, "%.1f m/s", d.speed);
            ImGui::TextUnformatted(b);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.0f km/h over the ground%s; vertical %s", d.speed * 3.6, d.direction >= 0 ? ", track true" : "",
                                                          d.vspeedValid ? (std::to_string((int)std::lround(d.vspeed * 10) / 10.0).substr(0, 5) + " m/s").c_str() : "unknown");
        } else ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        if (d.hasOperator) { ImGui::PushFont(a.mono, 0); ImGui::Text("%.6f %.6f", d.opLat, d.opLon); ImGui::PopFont(); } else ImGui::TextDisabled("-");
        if (d.hasOperator && ImGui::IsItemHovered()) ImGui::SetTooltip("operator position: %s, altitude %s", dect2::rid::opLocTypeName(d.opLocType), altText(d.opAlt).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(d.viaBle && d.viaWifi ? "BLE + Wi-Fi" : d.viaBle ? "BLE" : "Wi-Fi");
        ImGui::TableNextColumn();
        ImGui::Text("%4.0f", d.rssi);
        if (d.rssiHist.size() >= 2) {
            ImGui::SameLine(0, 4 * gUi);
            ImGui::PlotLines("##h", d.rssiHist.data(), (int)d.rssiHist.size(), 0, nullptr, -110, -10, ImVec2(48 * gUi, ImGui::GetTextLineHeight()));
        }
        ImGui::TableNextColumn(); ImGui::Text(ago < 1 ? "now" : "%.0f s ago", ago);
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (t.drones.empty()) ImGui::TextDisabled("no drones heard yet");
    else if (!shown) ImGui::TextDisabled("no drone passes the filter");
}

ImU32 droneColour(const dect2::RidDrone& d, double ago, float alpha = 1) {
    const int al = (int)(alpha * 255);
    if (d.status == 3 || d.status == 4) return IM_COL32(240, 150, 40, al);
    return ago < 10 ? IM_COL32(110, 220, 230, al) : IM_COL32(150, 160, 165, al);
}

void mapView(App& a, ImVec2 size) {
    const dect2::RidTelemetry& t = a.rx.rid;
    const bool on = live(a);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (S.mapFollow && on) {   // fit the drones and operators, until the user moves the map
        double la0 = 90, la1 = -90, lo0 = 180, lo1 = -180; int n = 0;
        auto add = [&](double la, double lo) { la0 = std::min(la0, la); la1 = std::max(la1, la); lo0 = std::min(lo0, lo); lo1 = std::max(lo1, lo); n++; };
        for (const auto& d : t.drones) { if (d.hasPos) add(d.lat, d.lon); if (d.hasOperator) add(d.opLat, d.opLon); }
        if (n) {
            S.map.lat = (la0 + la1) / 2; S.map.lon = (lo0 + lo1) / 2;
            // the deepest zoom where both the width and the height fit (a degree of latitude is 1 / cos(lat) times longer on the map)
            const double zw = std::log2(0.7 * size.x * 360.0 / (256.0 * std::max(0.002, lo1 - lo0)));
            const double zh = std::log2(0.7 * size.y * 360.0 * std::cos(S.map.lat * 3.14159265358979 / 180) / (256.0 * std::max(0.001, la1 - la0)));
            S.map.zoom = std::max(2, std::min(17, (int)std::floor(std::min(zw, zh))));
        }
    }
    const double lat0 = S.map.lat, lon0 = S.map.lon; const int zoom0 = S.map.zoom;
    adsbmap::draw(S.map, size);
    if (S.map.lat != lat0 || S.map.lon != lon0 || S.map.zoom != zoom0) S.mapFollow = false;   // panned or zoomed (the wheel at the centre moves nothing)
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    const dect2::RidDrone* hit = nullptr; float best = 16 * gUi;
    ImVec2 click; const bool clicked = adsbmap::clickedAt(click);
    if (on) {
        for (const auto& d : t.drones) {   // trails and operator links first, so the marks sit on top
            const double ago = t.timeSec - d.lastSec;
            const bool sel = d.mac == S.sel;
            if (d.hasPos && d.track.size() >= 2) {
                ImVec2 prev = adsbmap::project(d.track[0].lat, d.track[0].lon);
                for (size_t i = 1; i < d.track.size(); i++) { const ImVec2 q = adsbmap::project(d.track[i].lat, d.track[i].lon); dl->AddLine(prev, q, droneColour(d, ago, 0.45f), 1.5f); prev = q; }
            }
            if (d.hasPos && d.hasOperator) {   // the operator, linked to its drone by a dashed line
                const ImVec2 q = adsbmap::project(d.lat, d.lon), o = adsbmap::project(d.opLat, d.opLon);
                const float len = std::hypot(o.x - q.x, o.y - q.y), dash = 6 * gUi;
                const ImU32 lc = sel ? IM_COL32(255, 255, 255, 220) : IM_COL32(230, 200, 90, 170);
                for (float s = 0; s < len; s += 2 * dash) {
                    const float e = std::min(len, s + dash);
                    dl->AddLine(ImVec2(q.x + (o.x - q.x) * s / len, q.y + (o.y - q.y) * s / len), ImVec2(q.x + (o.x - q.x) * e / len, q.y + (o.y - q.y) * e / len), lc, 1.5f * gUi);
                }
            }
            if (d.hasOperator) {
                const ImVec2 o = adsbmap::project(d.opLat, d.opLon);
                const float r = (sel ? 6.f : 5.f) * gUi;
                dl->AddRectFilled(ImVec2(o.x - r, o.y - r), ImVec2(o.x + r, o.y + r), IM_COL32(230, 200, 90, 230), 2.f);
                dl->AddRect(ImVec2(o.x - r, o.y - r), ImVec2(o.x + r, o.y + r), IM_COL32(0, 0, 0, 200), 2.f, 0, 1.5f);
                dl->AddText(ImVec2(o.x - ImGui::CalcTextSize("O").x * 0.5f, o.y - ImGui::GetTextLineHeight() * 0.5f), IM_COL32(20, 20, 20, 255), "O");
                if (clicked) { const float dd = std::hypot(o.x - click.x, o.y - click.y); if (dd < best) { best = dd; hit = &d; } }
            }
        }
        for (const auto& d : t.drones) {
            if (!d.hasPos) continue;
            const ImVec2 q = adsbmap::project(d.lat, d.lon);
            if (q.x < p0.x - 40 || q.y < p0.y - 40 || q.x > p0.x + size.x + 40 || q.y > p0.y + size.y + 40) continue;
            const bool sel = d.mac == S.sel;
            const ImU32 col = droneColour(d, t.timeSec - d.lastSec);
            const float r = (sel ? 9.f : 7.f) * gUi;
            // a cross of four rotors
            dl->AddLine(ImVec2(q.x - r, q.y - r), ImVec2(q.x + r, q.y + r), IM_COL32(0, 0, 0, 200), 4.f * gUi);
            dl->AddLine(ImVec2(q.x - r, q.y + r), ImVec2(q.x + r, q.y - r), IM_COL32(0, 0, 0, 200), 4.f * gUi);
            dl->AddLine(ImVec2(q.x - r, q.y - r), ImVec2(q.x + r, q.y + r), col, 2.f * gUi);
            dl->AddLine(ImVec2(q.x - r, q.y + r), ImVec2(q.x + r, q.y - r), col, 2.f * gUi);
            for (int k = 0; k < 4; k++) dl->AddCircle(ImVec2(q.x + ((k & 1) ? r : -r), q.y + ((k & 2) ? r : -r)), 3.f * gUi, col, 12, 1.5f * gUi);
            if (d.direction >= 0) {   // the track
                const float rad = d.direction * 3.14159265f / 180.f;
                dl->AddLine(q, ImVec2(q.x + std::sin(rad) * r * 2.2f, q.y - std::cos(rad) * r * 2.2f), col, 1.5f * gUi);
            }
            if (adsbmap::labelsAt(S.map.zoom, sel)) {
                const std::string s = idOf(d);
                char b2[96]; snprintf(b2, sizeof b2, "%s", altText(d.height).c_str());
                if (sel) { const std::string mine = mylocmap::distanceBearingText(d.lat, d.lon); if (!mine.empty()) snprintf(b2, sizeof b2, "%s  %s", altText(d.height).c_str(), mine.c_str()); }
                const ImVec2 ts = ImGui::CalcTextSize(s.c_str()), ts2 = ImGui::CalcTextSize(b2);
                const float w = std::max(ts.x, ts2.x) + 8 * gUi, hh = ts.y + ts2.y + 4 * gUi;
                const ImVec2 lp(q.x + 14 * gUi, q.y - hh * 0.5f);
                dl->AddRectFilled(lp, ImVec2(lp.x + w, lp.y + hh), IM_COL32(10, 12, 14, 210), 2.f);
                dl->AddRect(lp, ImVec2(lp.x + w, lp.y + hh), sel ? IM_COL32(255, 255, 255, 200) : IM_COL32(60, 64, 68, 255), 2.f);
                dl->AddText(ImVec2(lp.x + 4 * gUi, lp.y + 2 * gUi), IM_COL32(235, 238, 240, 255), s.c_str());
                dl->AddText(ImVec2(lp.x + 4 * gUi, lp.y + 2 * gUi + ts.y), col, b2);
            }
            if (clicked) { const float dd = std::hypot(q.x - click.x, q.y - click.y); if (dd < best) { best = dd; hit = &d; } }
        }
    }
    if (clicked) S.sel = hit ? hit->mac : std::string();
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) { S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1); S.mapFollow = false; } if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom in"));
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) { S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1); S.mapFollow = false; } if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Zoom out"));
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("fit")) S.mapFollow = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show every drone and operator heard");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the position itself");
    plat::prefs().setI("ridZoom", S.map.zoom);
    int withPos = 0, ops = 0;
    if (on) for (const auto& d : t.drones) { if (d.hasPos) withPos++; if (d.hasOperator) ops++; }
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
    if (!on) adsbmap::legend(size.x, "start the receiver");
    else if (!withPos) adsbmap::legend(size.x, "no drone positions yet");
    else adsbmap::legend(size.x, "%d drone%s with a position, %d operator%s (O, dashed line to the drone)", withPos, withPos == 1 ? "" : "s", ops, ops == 1 ? "" : "s");
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
    ImGui::Dummy(ImVec2(0, 0));   // an item after SetCursorScreenPos: ImGui otherwise reports "extend window/parent boundaries"
}

void messagesView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the messages"); return; }
    const auto& log = a.rx.rid.log;
    if (ImGui::Button("Copy")) {
        std::string txt;
        for (const auto& p : log) {
            char b[600];
            snprintf(b, sizeof b, "%s\t%s %d\t%s\t%.0f\t%s\n", clock(p.wallTime).c_str(), p.transport ? "Wi-Fi" : "BLE", p.channel, p.mac.c_str(), p.rssi, p.info.c_str());
            txt += b;
        }
        ImGui::SetClipboardText(txt.c_str());
    }
    if (!ImGui::BeginTable("##ridlog", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(dect2::timefmt::headTime()); ImGui::TableSetupColumn("Via"); ImGui::TableSetupColumn("Address"); ImGui::TableSetupColumn("Level");
    ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGuiListClipper clip;
    clip.Begin((int)log.size());
    while (clip.Step())
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const auto& p = log[(size_t)i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(p.wallTime).c_str());
            ImGui::TableNextColumn(); if (p.channel) ImGui::Text("%s %d", p.transport ? "Wi-Fi" : "BLE", p.channel); else ImGui::TextUnformatted(p.transport ? "Wi-Fi" : "BLE");
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(p.mac.c_str()); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::Text("%.0f", p.rssi);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.info.c_str());
        }
    ImGui::EndTable();
    if (log.empty()) ImGui::TextDisabled("no messages yet");
}

// ---------------------------------------------------------------- hooks

void tuner(App& a, bool& retune) {
    loadState();
    struct Band { double mhz; const char* name; };
    static const Band kBands[] = {{2432, "BLE 38 + Wi-Fi 6 (2432 MHz)"}, {2437, "Wi-Fi 6 (2437 MHz)"}, {2412, "Wi-Fi 1 (2412 MHz)"}, {2462, "Wi-Fi 11 (2462 MHz)"},
                                  {2403, "BLE 37 + Wi-Fi 1 (2403 MHz)"}, {2426, "BLE 38 (2426 MHz)"}, {2478, "BLE 39 (2478 MHz)"}};
    ImGui::TextDisabled("Band");
    const char* label = "other";
    for (const auto& b : kBands) if (std::fabs(a.freqMhz - b.mhz) < 0.05) label = b.name;
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(210 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##ridb", label)) {
        for (const auto& b : kBands) if (ImGui::Selectable(b.name, std::fabs(a.freqMhz - b.mhz) < 0.05)) { tuneFreq(a, b.mhz); retune = true; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        const dect2::RidPlan p = dect2::ridPlan(a.freqMhz * 1e6);
        char ble[48] = "BLE not in the band";
        if (p.bleChannel) snprintf(ble, sizeof ble, "BLE channel %d at %+.0f MHz", p.bleChannel, p.bleOffsetHz / 1e6);
        ImGui::SetTooltip("One 20 MHz band of 2.4 GHz is heard at a time. Drones advertise on all three BLE channels, and most send Wi-Fi beacons on channel 6,\n"
                          "so 2432 MHz hears both. Now: %s, Wi-Fi channel %d at %+.0f MHz.", ble, p.wifiChannel, p.wifiOffsetHz / 1e6);
    }
}

// Airspace alerts (3.9, docs/SMART5-avia-3.9.md): above 120 m, within 5 km of an airport, near a manned aircraft that ADS-B hears at the
// same time (only when this app has live ADS-B telemetry next to the drones, as the second mode). The drone is reported, not its operator.
dect2::DroneAirspaceWatch gRidWatch;
std::deque<dect2::DroneAlert> gRidAlerts;
std::unordered_map<std::string, double> gRidFed;   // the last signal time fed per drone
void tick(App& a) {
    if (!live(a)) return;
    if (!gRidWatch.nearestAirport) gRidWatch.nearestAirport = [](double lat, double lon, std::string& code, double& km) {
        for (const auto& h : dect2::AirportDb::builtin().nearby(lat, lon, 10, 20, false)) {
            const auto& ap = dect2::AirportDb::builtin().all()[h.index];
            if (ap.kind == 'L' || ap.kind == 'M' || ap.scheduled) { code = ap.icao; km = h.km; return true; }   // airports with traffic, not every strip
        }
        return false;
    };
    const double now = ImGui::GetTime();
    const dect2::AdsbTelemetry* ad = a.second.rx.standard == 12 ? &a.second.rx.adsb : nullptr;   // ADS-B as the second mode
    if (ad) for (const auto& ac : ad->aircraft) {
        if (!ac.hasPos || !ac.hasAlt || ac.posAgeSec > 5) continue;
        dect2::MannedFix m; m.icao = ac.icao; m.callsign = ac.callsign; m.t = now - ac.posAgeSec; m.lat = ac.lat; m.lon = ac.lon;
        m.altM = (ac.hasGeoAlt ? ac.geoAltFt : ac.altFt) * 0.3048;
        gRidWatch.aircraft(m);
    }
    std::vector<dect2::DroneAlert> out;
    for (const auto& d : a.rx.rid.drones) {
        if (!d.hasPos) continue;
        const std::string id = !d.serial.empty() ? d.serial : !d.sessionId.empty() ? d.sessionId : d.mac;
        double& fed = gRidFed[id];
        if (d.lastSec == fed) continue;
        fed = d.lastSec;
        dect2::DroneFix f; f.id = id; f.t = now; f.lat = d.lat; f.lon = d.lon; f.heightM = d.height; f.altM = d.altGeo > -1000 ? d.altGeo : d.altBaro;
        gRidWatch.drone(f, out);
    }
    for (auto& x : out) { gRidAlerts.push_back(x); if (gRidAlerts.size() > 50) gRidAlerts.pop_front(); }
}
void alertsView() {
    { ImGui::PushTextWrapPos(0);
      ImGui::TextDisabled("%s", tr("Drones above 120 m, within 5 km of an airport, or near a manned aircraft heard on ADS-B at the same time (needs ADS-B running as the second mode). Each drone and reason once per 10 minutes."));
      ImGui::PopTextWrapPos(); }
    if (gRidAlerts.empty()) { ImGui::TextDisabled("%s", tr("none")); return; }
    for (auto it = gRidAlerts.rbegin(); it != gRidAlerts.rend(); ++it) ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.25f, 1), "%s", it->text.c_str());
}

void tab(App& a) {
    loadState();
    subNav("ridv", S.view, {"Drones", "Map", "Messages", "Alerts"});
    if (S.view == 0) dronesView(a);
    else if (S.view == 1) { const ImVec2 av = ImGui::GetContentRegionAvail(); if (av.x > 60 && av.y > 60) mapView(a, av); ImGui::Dummy(ImVec2(0, 0)); }
    else if (S.view == 3) alertsView();
    else messagesView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::RidTelemetry& t = a.rx.rid;
    if (!ImGui::BeginTable("##ridl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Drone", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Height");
    for (const auto& d : t.drones) {
        ImGui::TableNextRow();
        ImGui::PushID(d.mac.c_str());
        const bool recent = t.timeSec - d.lastSec < 3;
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, recent ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text));
        if (ImGui::Selectable(idOf(d).c_str(), S.sel == d.mac)) S.sel = d.mac;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s, %s\n%s", d.mac.c_str(), d.uaType < 0 ? "UA type not heard" : dect2::rid::uaTypeName(d.uaType),
                                                      d.operatorId.empty() ? "no operator ID" : d.operatorId.c_str());
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", altText(d.height).c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::RidTelemetry& t = a.rx.rid;
    kvRow(a, "state", "%s", dect2::ridSummary(t).c_str());
    if (t.bleChannel) kvRow(a, "Bluetooth LE", "channel %d (%.0f MHz, %+.1f MHz from the centre)", t.bleChannel, dect2::ble::channelMhz(t.bleChannel), t.bleOffsetHz / 1e6);
    else kvRow(a, "Bluetooth LE", t.centerMhz > 0 ? "no advertising channel in the band" : "at 0 Hz (a file)");
    if (t.wifiChannel) kvRow(a, "Wi-Fi", "channel %d (%.0f MHz, %+.1f MHz from the centre)", t.wifiChannel, dect2::wifi::channelMhz(t.wifiChannel), t.wifiOffsetHz / 1e6);
    else kvRow(a, "Wi-Fi", "at 0 Hz (a file)");
    kvRow(a, "packets", "BLE %llu good, %llu with Remote ID; Wi-Fi %llu good, %llu with Remote ID; %llu failed CRC / FCS", (unsigned long long)t.blePackets,
          (unsigned long long)t.bleRid, (unsigned long long)t.wifiFrames, (unsigned long long)t.wifiRid, (unsigned long long)t.blocksBad);
    kvRow(a, "messages", "Basic ID %llu, location %llu, authentication %llu, Self-ID %llu, system %llu, operator ID %llu", (unsigned long long)t.perType[0],
          (unsigned long long)t.perType[1], (unsigned long long)t.perType[2], (unsigned long long)t.perType[3], (unsigned long long)t.perType[4], (unsigned long long)t.perType[5]);
    kvRow(a, "signal", "SNR %.1f dB, offset %+.1f kHz", t.snrDb, t.cfoHz / 1e3);
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
    lamp("Remote ID", !on ? 0 : a.rx.rid.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::ridSummary(a.rx.rid).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Drone Remote ID";
    if (live(a)) l2 = dect2::ridSummary(a.rx.rid);
}

} // namespace

extern const ModeUi kRidUi;
const ModeUi kRidUi = {
    .sideTitle = "DRONES",
    .tabName = "Drones",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .tick = tick,
};
