// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Send the selected sonde as NMEA to a serial port": the controls (in the radiosonde screens' "me / prediction" popup) and the once-a-second
// feed. Core: dect2/sonde/sonde_nmea.h (the sentences), sonde_nmea_out.h (serial port, TCP, UDP). Off until ticked.
#include "app.h"
#include "dect2/source/rig_setup.h"
#include "dect2/sonde/sonde_nmea.h"
#include "dect2/sonde/sonde_nmea_out.h"
#include <chrono>
#include <cstdio>
#include <ctime>

void sondeNmeaControls();
void sondeNmeaTick(const dect2::SondeInfo* sel);

namespace {
struct Ns {
    bool loaded = false;
    dect2::SondeNmeaConfig c;
    char path[256] = {}, host[128] = {};
    std::vector<dect2::SerialPortInfo> ports;
    double portsT = -1e9;
    double lastSendT = -1e9, lastChangeT = -1e9;
    std::string lastSerial; int lastFrame = -2; double lastLat = 0, lastAlt = 0;
};
Ns& N() { static Ns* n = new Ns(); return *n; }
double steady() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void load() {
    Ns& n = N();
    if (n.loaded) return;
    n.loaded = true;
    plat::Prefs& p = plat::prefs();
    n.c.enabled = p.getB("sondeNmeaOn", false);
    n.c.kind = std::max(0, std::min(2, (int)p.getI("sondeNmeaKind", 0)));
    n.c.baud = (int)p.getI("sondeNmeaBaud", 4800);
    n.c.port = std::max(1, std::min(65535, (int)p.getI("sondeNmeaPort", 10110)));
    std::snprintf(n.path, sizeof n.path, "%s", p.getS("sondeNmeaPath", "").c_str());
    std::snprintf(n.host, sizeof n.host, "%s", p.getS("sondeNmeaHost", "127.0.0.1").c_str());
    n.c.path = n.path; n.c.host = n.host;
    if (n.c.enabled) dect2::SondeNmeaOut::get().configure(n.c);
}
void apply() {
    Ns& n = N();
    n.c.path = n.path; n.c.host = n.host;
    plat::Prefs& p = plat::prefs();
    p.setB("sondeNmeaOn", n.c.enabled); p.setI("sondeNmeaKind", n.c.kind); p.setI("sondeNmeaBaud", n.c.baud); p.setI("sondeNmeaPort", n.c.port);
    p.setS("sondeNmeaPath", n.path); p.setS("sondeNmeaHost", n.host);
    dect2::SondeNmeaOut::get().configure(n.c);
}
}  // namespace

void sondeNmeaControls() {
    load();
    Ns& n = N();
    bool ch = false;
    ImGui::TextDisabled("%s", tr("NMEA GPS output of the selected sonde"));
    ch |= ImGui::Checkbox(tr("Send the selected sonde as NMEA (GPS sentences)##sondenmea"), &n.c.enabled);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("For a map program that takes a GPS over a serial port. Sends $GPGGA and $GPRMC once a second for the sonde chosen in the list, and nothing while it has no position. Off by default."));
    ImGui::SetNextItemWidth(220 * gUi);
    ch |= ImGui::RadioButton(tr("Serial port##nmk0"), &n.c.kind, 0); ImGui::SameLine();
    ch |= ImGui::RadioButton(tr("TCP##nmk1"), &n.c.kind, 1); ImGui::SameLine();
    ch |= ImGui::RadioButton(tr("UDP##nmk2"), &n.c.kind, 2);
    if (n.c.kind == 0) {
        const double now = steady();
        if (now - n.portsT > 5) { n.ports = dect2::listSerialPorts(); n.portsT = now; }
        ImGui::SetNextItemWidth(260 * gUi);
        if (ImGui::BeginCombo(tr("Port##nmport"), n.path[0] ? n.path : tr("choose..."))) {
            for (const auto& p : n.ports) if (ImGui::Selectable(p.display.c_str(), p.path == n.path)) { std::snprintf(n.path, sizeof n.path, "%s", p.path.c_str()); ch = true; }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(260 * gUi);
        if (ImGui::InputText(tr("or type a port, a virtual COM port or a pty##nmpath"), n.path, sizeof n.path, ImGuiInputTextFlags_EnterReturnsTrue)) ch = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Windows COM3, macOS /dev/cu.usbserial-..., Linux /dev/ttyUSB0 or a pty from socat / com0com. Press Enter."));
        static const int bauds[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::BeginCombo(tr("Baud rate##nmbaud"), std::to_string(n.c.baud).c_str())) {
            for (int b : bauds) if (ImGui::Selectable(std::to_string(b).c_str(), b == n.c.baud)) { n.c.baud = b; ch = true; }
            ImGui::EndCombo();
        }
    } else {
        ImGui::SetNextItemWidth(160 * gUi);
        if (ImGui::InputText(tr("host##nmhost"), n.host, sizeof n.host, ImGuiInputTextFlags_EnterReturnsTrue)) ch = true;
        ImGui::SameLine(); ImGui::SetNextItemWidth(80 * gUi);
        if (ImGui::InputInt(tr("port##nmtcp"), &n.c.port, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) { n.c.port = std::max(1, std::min(65535, n.c.port)); ch = true; }
    }
    if (ch) apply();
    if (n.c.enabled) ImGui::TextDisabled("%s", dect2::SondeNmeaOut::get().status().c_str());
}

// every frame from the sonde screens' tick: once a second the sentences of the selected sonde; nothing for no sonde, no position or a sonde
// that has not sent a new frame for 15 s
void sondeNmeaTick(const dect2::SondeInfo* sel) {
    load();
    Ns& n = N();
    if (!n.c.enabled) return;
    const double now = steady();
    if (!sel) return;
    if (sel->serial != n.lastSerial || sel->frame != n.lastFrame || sel->lat != n.lastLat || sel->altM != n.lastAlt) {
        n.lastSerial = sel->serial; n.lastFrame = sel->frame; n.lastLat = sel->lat; n.lastAlt = sel->altM; n.lastChangeT = now;
    }
    if (now - n.lastChangeT > 15 || now - n.lastSendT < 1.0) return;
    const std::string t = dect2::sondenmea::sentences(*sel, (double)std::time(nullptr), 1e9);
    if (t.empty()) return;
    n.lastSendT = now;
    dect2::SondeNmeaOut::get().send(t);
}
