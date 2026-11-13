// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Radiosonde chase: the live position of a moving receiver (chase car) for the sonde map and the SondeHub station record, and the
// "my station" mark of the map. Core: dect2/sonde/sonde_chase.h (NMEA serial or file, gpsd). Off until a source is chosen.
#include "app.h"
#include "state/receivers.h"
#include "dect2/sonde/sonde_chase.h"
#include "dect2/sonde/sonde_map_overlay.h"
#include <cstdio>

void sondeChaseControls(App& a);
bool sondeStationMark(App& a, dect2::SondeMapStation& st);

namespace {
struct Cs { bool loaded = false; int src = 0; char path[256] = {}; char host[128] = "127.0.0.1"; int port = 2947; bool showMe = true; dect2::ChaseGps gps; };
Cs& C() { static Cs* c = new Cs(); return *c; }   // never destroyed: the GPS thread may still run at exit
void load() {
    Cs& c = C();
    if (c.loaded) return;
    c.loaded = true;
    plat::Prefs& p = plat::prefs();
    c.src = std::max(0, std::min(2, (int)p.getI("sondeChaseSrc", 0)));
    std::snprintf(c.path, sizeof c.path, "%s", p.getS("sondeChasePath", "").c_str());
    std::snprintf(c.host, sizeof c.host, "%s", p.getS("sondeChaseHost", "127.0.0.1").c_str());
    c.port = (int)p.getI("sondeChasePort", 2947);
    c.showMe = p.getB("sondeShowMe", true);
    if (c.src == 1) c.gps.start(dect2::ChaseGps::Nmea, c.path);
    else if (c.src == 2) c.gps.start(dect2::ChaseGps::Gpsd, c.host, c.port);
}
void save() {
    Cs& c = C();
    plat::Prefs& p = plat::prefs();
    p.setI("sondeChaseSrc", c.src); p.setS("sondeChasePath", c.path); p.setS("sondeChaseHost", c.host); p.setI("sondeChasePort", c.port); p.setB("sondeShowMe", c.showMe);
}
void restart() {
    Cs& c = C();
    if (c.src == 1) c.gps.start(dect2::ChaseGps::Nmea, c.path);
    else if (c.src == 2) c.gps.start(dect2::ChaseGps::Gpsd, c.host, c.port);
    else c.gps.stop();
    save();
}
}  // namespace

bool sondeChasePosition(double& lat, double& lon, double& altM) {
    load();
    dect2::ChaseFix f;
    if (!C().gps.fix(f)) return false;
    lat = f.lat; lon = f.lon; altM = f.hasAlt ? f.altM : receiverAltitudeM();
    return true;
}

// the station mark of the map: the chase GPS while it has a fix, else the location set in View > Map; labelled with the callsign
bool sondeStationMark(App& a, dect2::SondeMapStation& st) {
    load();
    st = dect2::SondeMapStation();
    if (!C().showMe) return false;
    const App* f = receiverById(1); if (!f) f = &a;
    st.label = !f->feeds.sondeHub.call.empty() ? f->feeds.sondeHub.call : f->feeds.aprsCall;
    double alt = 0;
    if (sondeChasePosition(st.lat, st.lon, alt)) { st.set = true; st.live = true; return true; }
    if (!receiverLocationSet()) return false;
    receiverLocation(st.lat, st.lon);
    st.set = true;
    return true;
}

void sondeChaseControls(App& a) {
    (void)a;
    load();
    Cs& c = C();
    if (ImGui::Checkbox(tr("Show my station on the map##sondeme"), &c.showMe)) save();
    ImGui::TextDisabled("%s", tr("My position:"));
    bool ch = false;
    ch |= ImGui::RadioButton(tr("Location set in View > Map##chs0"), &c.src, 0); ImGui::SameLine();
    ch |= ImGui::RadioButton(tr("GPS (NMEA)##chs1"), &c.src, 1); ImGui::SameLine();
    ch |= ImGui::RadioButton(tr("gpsd##chs2"), &c.src, 2);
    if (c.src == 1) {
        ImGui::SetNextItemWidth(220 * gUi);
        if (ImGui::InputText(tr("device or file##chpath"), c.path, sizeof c.path, ImGuiInputTextFlags_EnterReturnsTrue)) ch = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A serial GPS (macOS /dev/cu.usbmodem..., Linux /dev/ttyACM0 or /dev/ttyUSB0, 9600 baud; Windows \\\\.\\COM3) or an NMEA log file. Press Enter."));
    } else if (c.src == 2) {
        ImGui::SetNextItemWidth(160 * gUi);
        if (ImGui::InputText(tr("host##chhost"), c.host, sizeof c.host, ImGuiInputTextFlags_EnterReturnsTrue)) ch = true;
        ImGui::SameLine(); ImGui::SetNextItemWidth(80 * gUi);
        if (ImGui::InputInt(tr("port##chport"), &c.port, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) { c.port = std::max(1, std::min(65535, c.port)); ch = true; }
    }
    if (ch) restart();
    if (c.src != 0) {
        dect2::ChaseFix f;
        if (c.gps.fix(f)) ImGui::TextColored(pal::okGreen(), "%s: %.5f, %.5f%s", c.gps.status().c_str(), f.lat, f.lon, f.hasAlt ? (", " + std::to_string((int)f.altM) + " m").c_str() : "");
        else ImGui::TextDisabled("%s", c.gps.status().c_str());
        ImGui::TextDisabled("%s", tr("With a GPS fix the map shows a chase car, and SondeHub's Chase car option sends this position."));
    } else if (!receiverLocationSet()) ImGui::TextDisabled("%s", tr("No location set yet: View > Map, Use as my location."));
}
