// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DJI drones screens: the signals seen by their shape (class, confidence, frequency, width, burst period, duty cycle, level over time), the DroneID packets
// decoded (serial number, position, home point, pilot position as sent in the clear), the burst log, and the radio notes.
// Receive only. Nothing is transmitted and the video link (encrypted and proprietary) is not decoded. A class from the shape is a guess with a
// confidence, never a certain identification; only a DroneID packet that passes its CRC is one. The standard Remote ID broadcast (Wi-Fi / BLE) is the
// Drone Remote ID mode.
#include "app.h"
#include "dect2/dji/dji_gen.h"
#include "dect2/dji/dji_rx.h"
#include "dect2/dji/dji_tel.h"
#include "dect2/rid/rid_alerts.h"
#include "dect2/airports/airports.h"
#include "../smart5/smart5_ui.h"
#include <deque>
#include <unordered_map>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

struct State {
    bool loaded = false, pushed = false;
    int view = 0;                        // 0 signals, 1 DroneID, 2 bursts
    int selTrack = 0;
    std::string selDrone;
    bool decode = true;
};
State S;

void saveState() { plat::prefs().setB("djiDecode", S.decode); S.pushed = false; }

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.decode = plat::prefs().getB("djiDecode", true);
}

bool live(const App& a) { return a.rx.standard == 69; }   // the engine reports its standard code minus one

std::string clock(int64_t t, double sigSec) {
    char b[24];
    if (t <= 0) { snprintf(b, sizeof b, "%.1f s", sigSec); return b; }
    const time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    snprintf(b, sizeof b, "%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

const char* kShapeNote = "Detected by signal shape, not a certain identification: a class is a guess from the width, burst timing and OFDM-like structure, with a confidence. "
                         "Only a DroneID packet that passes its CRC identifies a drone.";

void receiveNote() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Receive only: nothing is transmitted and nothing is decrypted. The video link of DJI drones is encrypted and proprietary: it is only recognised by its shape. "
                        "DroneID is a clear broadcast of the drone's serial number and position; it is shown only as received, kept for this session (and in the results file only if you keep results). "
                        "Check the rules for receiving radio signals where you are. The standard Remote ID broadcast (Wi-Fi / Bluetooth) is the Drone Remote ID mode.");
    ImGui::PopTextWrapPos();
}

ImVec4 classColor(int c) {
    switch (c) {
    case dect2::kDjiDroneId: return pal::okGreen();
    case dect2::kDjiOcusync: case dect2::kDjiLightbridge: return pal::accent();
    case dect2::kDjiUnknownOfdm: return pal::warnAmber();
    default: return pal::grey(0.55f);
    }
}

std::string freqText(const dect2::DjiTrack& k) {
    char b[64];
    if (k.freqMhz > 0) snprintf(b, sizeof b, "%.1f MHz %s", k.freqMhz, k.band.c_str());
    else snprintf(b, sizeof b, "%+.2f MHz", k.offsetHz / 1e6);
    return b;
}

void trackDetail(const App& a, const dect2::DjiTrack& k) {
    kvRow(a, "class", "%s, confidence %.0f%%", dect2::djiClassName(k.cls).c_str(), k.conf * 100);
    kvRow(a, "why", "%s", k.note.c_str());
    kvRow(a, "frequency", "%s (%+.2f MHz from the centre)", freqText(k).c_str(), k.offsetHz / 1e6);
    kvRow(a, "width", "%.1f MHz%s", k.bwMhz, k.clipped ? " (fills the capture: it may be wider)" : "");
    kvRow(a, "bursts", "%d, %.0f us long, every %.2f ms (regularity %.0f%%), on %.0f%% of the time", k.bursts, k.durUs, k.periodMs, k.periodQ * 100, k.duty * 100);
    kvRow(a, "OFDM-like", "%.2f (cyclic prefix correlation at 15 / 30 kHz spacing; noise and Wi-Fi are below 0.35)", k.ofdm);
    kvRow(a, "level", "%.1f dBFS, SNR %.1f dB over the capture", k.levelDb, k.snrDb);
    if (k.zc) kvRow(a, "DroneID", "Zadoff-Chu symbols found; %d packets decoded, %d failed their CRC", k.decoded, k.failed);
    if (k.rssi.size() >= 2) {
        ImGui::TextDisabled("level over time (one point per 0.25 s with a burst)");
        float lo = FLT_MAX, hi = -FLT_MAX;
        for (float v : k.rssi) { lo = std::min(lo, v); hi = std::max(hi, v); }
        ImGui::PlotLines("##djr", k.rssi.data(), (int)k.rssi.size(), 0, nullptr, lo - 3, hi + 3, ImVec2(std::min(420 * gUi, ImGui::GetContentRegionAvail().x), 70 * gUi));
    }
}

void signalsView(App& a) {
    receiveNote();
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(pal::warnAmber(), "%s", kShapeNote);
    ImGui::PopTextWrapPos();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver (20 Msps or more) to see the wideband signals around you"); return; }
    const dect2::DjiTelemetry& t = a.rx.dji;
    if (ImGui::SmallButton("Clear")) { a.engine.dji().clearResults(); S.selTrack = 0; S.selDrone.clear(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forget every signal, burst and drone heard so far");
    flowNext(10 * gUi);
    ImGui::TextDisabled("capture level, strongest burst per 0.25 s");
    if (t.levelHist.size() >= 2) ImGui::PlotLines("##djl", t.levelHist.data(), (int)t.levelHist.size(), 0, nullptr, -90.f, 0.f, ImVec2(std::min(420 * gUi, ImGui::GetContentRegionAvail().x), 46 * gUi));
    const float h = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginTable("##djt", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                          ImVec2(0, S.selTrack ? h * 0.4f : h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Class"); ImGui::TableSetupColumn("Conf."); ImGui::TableSetupColumn("Frequency"); ImGui::TableSetupColumn("Width");
        ImGui::TableSetupColumn("Burst"); ImGui::TableSetupColumn("Period"); ImGui::TableSetupColumn("Duty"); ImGui::TableSetupColumn("Level");
        ImGui::TableSetupColumn("Last seen", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& k : t.tracks) {
            ImGui::TableNextRow();
            ImGui::PushID(k.id);
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, classColor(k.cls));
            if (ImGui::Selectable(dect2::djiClassName(k.cls).c_str(), S.selTrack == k.id, ImGuiSelectableFlags_SpanAllColumns)) S.selTrack = S.selTrack == k.id ? 0 : k.id;
            ImGui::PopStyleColor();
            ImGui::TableNextColumn(); ImGui::Text("%.0f%%", k.conf * 100);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(freqText(k).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.1f MHz%s", k.bwMhz, k.clipped ? "+" : "");
            ImGui::TableNextColumn(); ImGui::Text("%.0f us", k.durUs);
            ImGui::TableNextColumn(); if (k.periodMs > 0) ImGui::Text("%.2f ms", k.periodMs); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); if (k.periodMs > 0) ImGui::Text("%.0f%%", k.duty * 100); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%.0f dBFS", k.levelDb);
            ImGui::TableNextColumn(); ImGui::Text("%.1f s ago", std::max(0.0, t.timeSec - k.lastSec));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (t.tracks.empty()) ImGui::TextDisabled("no wideband burst yet: a drone must be flying and transmitting inside the 20 MHz that are tuned");
    if (!S.selTrack) return;
    for (const auto& k : t.tracks)
        if (k.id == S.selTrack) {
            ImGui::Separator();
            if (ImGui::BeginChild("##djd", ImVec2(0, 0))) trackDetail(a, k);
            ImGui::EndChild();
            return;
        }
}

// The drone, its home point and the pilot, in metres from the drone's first position this session (a flat approximation for a few kilometres)
void trailPlot(const dect2::DjiDrone& d, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(24, 28, 34, 255));
    const dect2::dji::DroneId& id = d.last;
    struct Pt { double lat, lon; ImU32 col; const char* what; };
    std::vector<Pt> pts;
    for (const auto& q : d.track) pts.push_back({q.lat, q.lon, IM_COL32(110, 214, 196, 255), nullptr});
    if (std::abs(id.homeLat()) > 1e-4) pts.push_back({id.homeLat(), id.homeLon(), IM_COL32(236, 200, 90, 255), "home"});
    if (std::abs(id.appLat()) > 1e-4) pts.push_back({id.appLat(), id.appLon(), IM_COL32(236, 128, 140, 255), "pilot"});
    if (pts.empty()) { dl->AddText(ImVec2(p0.x + 8, p0.y + 8), IM_COL32(150, 160, 170, 255), "no position (no GPS lock yet)"); return; }
    const double lat0 = pts[0].lat, lon0 = pts[0].lon, kx = 111320.0 * std::cos(lat0 * 3.14159265 / 180), ky = 110574.0;
    double xmin = 1e18, xmax = -1e18, ymin = 1e18, ymax = -1e18;
    for (auto& q : pts) { const double x = (q.lon - lon0) * kx, y = (q.lat - lat0) * ky; xmin = std::min(xmin, x); xmax = std::max(xmax, x); ymin = std::min(ymin, y); ymax = std::max(ymax, y); }
    const double span = std::max({xmax - xmin, ymax - ymin, 20.0});
    const double sc = (std::min(size.x, size.y) - 24) / span;
    auto at = [&](const Pt& q) { return ImVec2((float)(p0.x + 12 + ((q.lon - lon0) * kx - xmin) * sc), (float)(p0.y + size.y - 12 - ((q.lat - lat0) * ky - ymin) * sc)); };
    ImVec2 prev;
    bool has = false;
    for (size_t i = 0; i < pts.size(); i++) {
        const ImVec2 c = at(pts[i]);
        if (!pts[i].what) {
            if (has) dl->AddLine(prev, c, IM_COL32(110, 214, 196, 160), 1.5f);
            prev = c; has = true;
            if (i + 1 == d.track.size()) dl->AddCircleFilled(c, 4.f * gUi, pts[i].col);
        } else {
            dl->AddCircleFilled(c, 4.f * gUi, pts[i].col);
            dl->AddText(ImVec2(c.x + 6, c.y - 6), pts[i].col, pts[i].what);
        }
    }
    char b[48];
    snprintf(b, sizeof b, "%.0f m across", span);
    dl->AddText(ImVec2(p0.x + 6, p0.y + size.y - 18), IM_COL32(150, 160, 170, 255), b);
}

void droneDetail(const App& a, const dect2::DjiDrone& d) {
    const dect2::dji::DroneId& id = d.last;
    kvRow(a, "serial", "%s", d.serial.empty() ? "(empty)" : d.serial.c_str());
    kvRow(a, "device type", "%d (the code the drone sends; the model table is not published)", id.deviceType);
    kvRow(a, "position", "%.6f, %.6f%s", id.lat(), id.lon(), std::abs(id.lat()) < 1e-4 ? "  (no GPS lock yet)" : "");
    kvRow(a, "altitude / height", "%.2f m / %.2f m (raw %d / %d; the scale is read from the published examples)", id.altitudeM(), id.heightM(), id.altRaw, id.heightRaw);
    kvRow(a, "speed (raw)", "north %d, east %d, up %d (the unit is not published)", id.vNorth, id.vEast, id.vUp);
    kvRow(a, "heading (raw)", "%d", id.yaw);
    kvRow(a, "home point", "%.6f, %.6f", id.homeLat(), id.homeLon());
    kvRow(a, "pilot (phone / controller)", "%.6f, %.6f", id.appLat(), id.appLon());
    kvRow(a, "GPS time", "%llu ms since 1970", (unsigned long long)id.gpsMs);
    kvRow(a, "packet", "sequence %d, state %04X, version %d, %s", id.seq, id.state, id.version, id.crc16Ok ? "inner CRC-16 ok" : "inner CRC-16 differs (the CRC-24 passed)");
    kvRow(a, "received", "%llu packets on %.1f MHz, SNR %.1f dB, last %s", (unsigned long long)d.packets, d.freqMhz, d.snrDb, clock(d.lastWall, d.lastSec).c_str());
    ImGui::Spacing();
    if (ImGui::SmallButton("Copy position")) { char b[80]; snprintf(b, sizeof b, "%.6f, %.6f", id.lat(), id.lon()); ImGui::SetClipboardText(b); }
    ImGui::Spacing();
    trailPlot(d, ImVec2(std::min(360 * gUi, ImGui::GetContentRegionAvail().x), 200 * gUi));
}

void droneIdView(App& a) {
    receiveNote();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("DJI DroneID is what OcuSync 2 drones (Mini 2, Mavic Air 2, Mavic 2, ...) broadcast about every 0.64 s: serial number, position, home point, pilot position. "
                        "Layout from the NDSS 2023 paper and the open notes of proto17/dji_droneid; the packet is decoded here only if its CRC-24 passes.");
    ImGui::PopTextWrapPos();
    if (ImGui::SmallButton("Open the Drone Remote ID mode")) selectMode(a, 56);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("DJI drones also broadcast the standard Remote ID (ASTM F3411) over Wi-Fi and Bluetooth: that is the other mode. Stop the receiver first to switch.");
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see DroneID packets"); return; }
    const dect2::DjiTelemetry& t = a.rx.dji;
    if (t.drones.empty()) {
        ImGui::TextDisabled("no DroneID packet yet (%llu failed their CRC). The burst is 10 MHz wide and comes every 0.64 s on one of a few frequencies in 2.4 and 5.8 GHz: the 20 MHz tuned must contain it.", (unsigned long long)t.blocksBad);
        return;
    }
    const float h = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginTable("##djdr", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX,
                          ImVec2(0, S.selDrone.empty() ? h : h * 0.3f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Serial"); ImGui::TableSetupColumn("Position"); ImGui::TableSetupColumn("Alt"); ImGui::TableSetupColumn("Height");
        ImGui::TableSetupColumn("Packets"); ImGui::TableSetupColumn("SNR"); ImGui::TableSetupColumn("Last seen", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& d : t.drones) {
            ImGui::TableNextRow();
            ImGui::PushID(d.serial.c_str());
            ImGui::TableNextColumn();
            if (ImGui::Selectable(d.serial.empty() ? "(empty)" : d.serial.c_str(), S.selDrone == d.serial, ImGuiSelectableFlags_SpanAllColumns)) S.selDrone = S.selDrone == d.serial ? "" : d.serial;
            ImGui::TableNextColumn();
            if (std::abs(d.last.lat()) > 1e-4) { ImGui::PushFont(a.mono, 0); ImGui::Text("%.6f %.6f", d.last.lat(), d.last.lon()); ImGui::PopFont(); } else ImGui::TextDisabled("no fix");
            ImGui::TableNextColumn(); ImGui::Text("%.1f m", d.last.altitudeM());
            ImGui::TableNextColumn(); ImGui::Text("%.1f m", d.last.heightM());
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)d.packets);
            ImGui::TableNextColumn(); ImGui::Text("%.0f dB", d.snrDb);
            ImGui::TableNextColumn(); ImGui::Text("%.1f s ago", std::max(0.0, t.timeSec - d.lastSec));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (S.selDrone.empty()) return;
    for (const auto& d : t.drones)
        if (d.serial == S.selDrone) {
            ImGui::Separator();
            if (ImGui::BeginChild("##djdd", ImVec2(0, 0))) droneDetail(a, d);
            ImGui::EndChild();
            return;
        }
}

void burstsView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the bursts"); return; }
    const dect2::DjiTelemetry& t = a.rx.dji;
    if (!ImGui::BeginTable("##djb", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Offset"); ImGui::TableSetupColumn("Width"); ImGui::TableSetupColumn("Length");
    ImGui::TableSetupColumn("Level"); ImGui::TableSetupColumn("SNR"); ImGui::TableSetupColumn("OFDM"); ImGui::TableSetupColumn("DroneID", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    static const char* zcText[] = {"-", "tried, no ZC symbols", "ZC found, CRC failed", "decoded"};
    for (const auto& b : t.bursts) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%s", clock(b.wallTime, b.timeSec).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%+.2f MHz", b.offsetHz / 1e6);
        ImGui::TableNextColumn(); ImGui::Text("%.1f MHz", b.bwMhz);
        ImGui::TableNextColumn(); ImGui::Text("%.0f us", b.durUs);
        ImGui::TableNextColumn(); ImGui::Text("%.0f dBFS", b.levelDb);
        ImGui::TableNextColumn(); ImGui::Text("%.0f dB", b.snrDb);
        ImGui::TableNextColumn(); ImGui::Text("%.2f", b.ofdm);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(zcText[std::max(0, std::min(3, b.zc))]);
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------- hooks

// Airspace alerts for the drones DJI DroneID reports (docs/SMART5-avia-3.9.md item 6, the Remote ID logic of rid_alerts.h): above 120 m, within
// 5 km of an airport, near a manned aircraft that ADS-B hears at the same time (ADS-B as the second mode). The drone is reported, not its pilot.
dect2::DroneAirspaceWatch gDjiWatch;
std::deque<dect2::DroneAlert> gDjiAlerts;
std::unordered_map<std::string, double> gDjiFed;
void alertsFeed(App& a) {
    if (!live(a)) return;
    if (!gDjiWatch.nearestAirport) gDjiWatch.nearestAirport = [](double lat, double lon, std::string& code, double& km) {
        for (const auto& h : dect2::AirportDb::builtin().nearby(lat, lon, 10, 20, false)) {
            const auto& ap = dect2::AirportDb::builtin().all()[h.index];
            if (ap.kind == 'L' || ap.kind == 'M' || ap.scheduled) { code = ap.icao; km = h.km; return true; }
        }
        return false;
    };
    const double now = ImGui::GetTime();
    const dect2::AdsbTelemetry* ad = a.second.rx.standard == 12 ? &a.second.rx.adsb : nullptr;
    if (ad) for (const auto& ac : ad->aircraft) {
        if (!ac.hasPos || !ac.hasAlt || ac.posAgeSec > 5) continue;
        dect2::MannedFix m; m.icao = ac.icao; m.callsign = ac.callsign; m.t = now - ac.posAgeSec; m.lat = ac.lat; m.lon = ac.lon;
        m.altM = (ac.hasGeoAlt ? ac.geoAltFt : ac.altFt) * 0.3048;
        gDjiWatch.aircraft(m);
    }
    std::vector<dect2::DroneAlert> out;
    for (const auto& d : a.rx.dji.drones) {
        if (d.last.latRaw == 0 && d.last.lonRaw == 0) continue;   // no GPS fix in the packet
        const std::string id = d.serial.empty() ? "DJI track " + std::to_string(d.trackId) : d.serial;
        double& fed = gDjiFed[id];
        if (d.lastSec == fed) continue;
        fed = d.lastSec;
        dect2::DroneFix f; f.id = id; f.t = now; f.lat = d.last.lat(); f.lon = d.last.lon(); f.heightM = d.last.heightM(); f.altM = d.last.altitudeM();
        gDjiWatch.drone(f, out);
    }
    for (auto& x : out) {
        gDjiAlerts.push_back(x); if (gDjiAlerts.size() > 50) gDjiAlerts.pop_front();
        s5ui::post("aviation", "drone_airspace", 1, x.text, "DJI " + x.text);   // View > Insights, insight.drone_airspace
    }
}
void alertsView() {
    { ImGui::PushTextWrapPos(0);
      ImGui::TextDisabled("%s", tr("Drones above 120 m, within 5 km of an airport, or near a manned aircraft heard on ADS-B at the same time (needs ADS-B running as the second mode). Each drone and reason once per 10 minutes."));
      ImGui::PopTextWrapPos(); }
    if (gDjiAlerts.empty()) { ImGui::TextDisabled("%s", tr("none")); return; }
    for (auto it = gDjiAlerts.rbegin(); it != gDjiAlerts.rend(); ++it) ImGui::TextColored(pal::warnAmber(), "%s", it->text.c_str());
}

void tick(App& a) {
    loadState();
    alertsFeed(a);
    if (S.pushed) return;
    S.pushed = true;
    a.engine.dji().setDecodeDroneId(S.decode);
}

void tab(App& a) {
    loadState();
    subNav("djv", S.view, {"Signals", "DroneID", "Bursts", gDjiAlerts.empty() ? tr("Alerts") : (std::string(tr("Alerts")) + " (" + std::to_string(gDjiAlerts.size()) + ")").c_str()});
    if (S.view == 0) signalsView(a); else if (S.view == 1) droneIdView(a); else if (S.view == 3) alertsView(); else burstsView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::DjiTelemetry& t = a.rx.dji;
    if (!ImGui::BeginTable("##djl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("Signal", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Conf.");
    for (const auto& d : t.drones) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, pal::okGreen());
        ImGui::PushID(d.serial.c_str());
        if (ImGui::Selectable(("DroneID " + d.serial).c_str(), S.selDrone == d.serial)) { S.selDrone = d.serial; S.view = 1; }
        ImGui::PopID();
        ImGui::PopStyleColor();
        ImGui::TableNextColumn(); ImGui::TextDisabled("CRC ok");
    }
    for (const auto& k : t.tracks) {
        if (k.cls == dect2::kDjiOther) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, classColor(k.cls));
        ImGui::PushID(k.id);
        char lb[96];
        snprintf(lb, sizeof lb, "%s  %.0f MHz", dect2::djiClassName(k.cls).c_str(), k.bwMhz);
        if (ImGui::Selectable(lb, S.selTrack == k.id)) { S.selTrack = k.id; S.view = 0; }
        ImGui::PopID();
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", freqText(k).c_str(), kShapeNote);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%.0f%%", k.conf * 100);
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::DjiTelemetry& t = a.rx.dji;
    kvRow(a, "state", "%s", dect2::djiSummary(t).c_str());
    kvRow(a, "bursts", "%llu wideband bursts measured, %llu OFDM-like", (unsigned long long)t.burstsTotal, (unsigned long long)t.burstsOfdm);
    kvRow(a, "DroneID", "%llu packets decoded, %llu with the ZC symbols but a failed CRC", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kvRow(a, "centre", t.centerMhz > 0 ? "%.3f MHz, %.0f MHz seen at once" : "unknown (a file), %.0f MHz seen at once", t.centerMhz > 0 ? t.centerMhz : t.inputRate / 1e6, t.inputRate / 1e6);
    kvRow(a, "noise floor", "%.1f dBFS per sample; last burst %.1f dBFS", t.noiseDb, t.levelDb);
    kvRow(a, "input", "%.3f Msps", t.inputRate / 1e6);
    kvRow(a, "signal time", "%.1f s", t.timeSec);
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("What each radio can see (the capture must hold the whole burst: 10 MHz for DroneID, 10 to 20 MHz for OcuSync-like links; at least 16 Msps, 20 Msps recommended): "
                        "HackRF One / Pro at 20 Msps: yes (20 MHz, the widest it gives); PlutoSDR (AD9361 hack, up to 61 Msps), bladeRF, USRP, LimeSDR, SDRplay RSPdx: yes; "
                        "Airspy R2 (10 Msps) and Mini (6 Msps), RTL-SDR, Airspy HF+: no. 40 MHz links can only be seen in part. "
                        "The tuner hears 20 MHz at a time: DroneID hops between a few channels, so a single window sees a fraction of its bursts.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    receiveNote();
}

void decoder(App& a, bool&) {
    loadState();
    if (ImGui::Checkbox("Decode DroneID", &S.decode)) { saveState(); tick(a); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Try every 0.5 - 1.5 ms burst that is 7 - 11 MHz wide as a DroneID burst: Zadoff-Chu synchronisation, turbo decoding, CRC-24.");
    flowNext();
    if (ImGui::SmallButton("Clear")) { a.engine.dji().clearResults(); S.selTrack = 0; S.selDrone.clear(); }
}

void synth(App& a, bool& changed) {
    dect2::SynthConfig& sc = a.tune.synth;
    static const char* kScen[] = {"everything", "DroneID only", "OcuSync-like only", "Lightbridge-like only", "Wi-Fi-like only", "DroneID with Wi-Fi collisions"};
    ImGui::TextDisabled("test signal");
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(210 * gUi, ImGui::GetContentRegionAvail().x));
    const int cur = std::max(0, std::min(5, sc.modeOpt[0]));
    if (ImGui::BeginCombo("##djsy", kScen[cur])) {
        for (int k = 0; k < 6; k++) if (ImGui::Selectable(kScen[k], cur == k)) { sc.modeOpt[0] = k; changed = true; }
        ImGui::EndCombo();
    }
    flowNext(8 * gUi);
    bool eight = sc.modeOpt[1] == 1;
    if (ImGui::Checkbox("8 symbols", &eight)) { sc.modeOpt[1] = eight ? 1 : 0; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("DroneID bursts without the first symbol, as the Mavic 2 and older OcuSync drones send them");
    flowNext(8 * gUi);
    bool wide = sc.modeOpt[2] == 1;
    if (ImGui::Checkbox("20 MHz", &wide)) { sc.modeOpt[2] = wide ? 1 : 0; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The OcuSync-like train 20 MHz wide, centred (the OcuSync-like and Lightbridge-like signals are stand-ins of the described shape, not the real protocols)");
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Bursts", !on ? 0 : a.rx.dji.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("DJI-like", !on ? 0 : a.rx.dji.state >= 2 ? 1 : 0); flowNext(10 * gUi);
    lamp("DroneID", !on ? 0 : a.rx.dji.state >= 3 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::djiSummary(a.rx.dji).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "DJI drones";
    if (live(a)) l2 = dect2::djiSummary(a.rx.dji);
}

} // namespace

extern const ModeUi kDjiUi;
const ModeUi kDjiUi = {
    .sideTitle = "SIGNALS",
    .tabName = "Signals",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .decoder = decoder,
    .synth = synth,
    .tick = tick,
};
