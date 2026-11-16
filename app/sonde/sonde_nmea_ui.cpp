// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Send the selected sonde as NMEA to a serial port" and the second output "Predicted course": the controls (in the radiosonde screens' "me / prediction" popup) and the once-a-second
// feed. Core: dect2/sonde/sonde_nmea.h (the sentences), sonde_nmea_out.h (serial port, TCP, UDP). Off until ticked.
#include "app.h"
#include "dect2/source/rig_setup.h"
#include "dect2/sonde/sonde_course.h"
#include "dect2/sonde/sonde_nmea.h"
#include "dect2/sonde/sonde_nmea_out.h"
#include <chrono>
#include <cstdio>
#include <ctime>

void sondeNmeaControls();
void sondeNmeaTick(const dect2::SondeInfo* sel);
bool sondePredictedCourse(const dect2::SondeInfo& s, std::vector<dect2::sondecourse::Point>& out);   // sonde_ui.cpp

namespace {
// One output: its settings (saved under prefs keys starting with pfx), its sender, and for the predicted-course one the sequence state.
struct Ns {
    const char* pfx = "";
    dect2::SondeNmeaOut* out = nullptr;
    bool loaded = false;
    dect2::SondeNmeaConfig c;
    char path[256] = {}, host[128] = {};
    std::vector<dect2::SerialPortInfo> ports;
    double portsT = -1e9;
    double lastSendT = -1e9, lastChangeT = -1e9;
    std::string lastSerial; int lastFrame = -2; double lastLat = 0, lastAlt = 0;
    // predicted course only
    int stepS = 60, everyS = 10;       // flight seconds between two points, real seconds between two sentences
    bool aimOnly = false;              // only the landing point, repeated
    dect2::sondecourse::Sequencer seq;
    double builtT = -1e9;
    std::string builtSerial;
};
Ns& N() { static Ns* n = new Ns(); if (!n->out) { n->pfx = "sondeNmea"; n->out = &dect2::SondeNmeaOut::get(); } return *n; }
Ns& C() { static Ns* n = new Ns(); if (!n->out) { n->pfx = "sondeNmeaPred"; n->out = &dect2::SondeNmeaOut::course(); } return *n; }
double steady() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::string key(const Ns& n, const char* k) { return std::string(n.pfx) + k; }
void load(Ns& n) {
    if (n.loaded) return;
    n.loaded = true;
    plat::Prefs& p = plat::prefs();
    n.c.enabled = p.getB(key(n, "On"), false);
    n.c.kind = std::max(0, std::min(2, (int)p.getI(key(n, "Kind"), 0)));
    n.c.baud = (int)p.getI(key(n, "Baud"), 4800);
    n.c.port = std::max(1, std::min(65535, (int)p.getI(key(n, "Port"), n.out == &dect2::SondeNmeaOut::get() ? 10110 : 10111)));
    std::snprintf(n.path, sizeof n.path, "%s", p.getS(key(n, "Path"), "").c_str());
    std::snprintf(n.host, sizeof n.host, "%s", p.getS(key(n, "Host"), "127.0.0.1").c_str());
    n.c.path = n.path; n.c.host = n.host;
    n.stepS = std::max(1, std::min(3600, (int)p.getI(key(n, "StepS"), 60)));
    n.everyS = std::max(1, std::min(3600, (int)p.getI(key(n, "EveryS"), 10)));
    n.aimOnly = p.getB(key(n, "Aim"), false);
    if (n.c.enabled) n.out->configure(n.c);
}
void apply(Ns& n) {
    n.c.path = n.path; n.c.host = n.host;
    plat::Prefs& p = plat::prefs();
    p.setB(key(n, "On"), n.c.enabled); p.setI(key(n, "Kind"), n.c.kind); p.setI(key(n, "Baud"), n.c.baud); p.setI(key(n, "Port"), n.c.port);
    p.setS(key(n, "Path"), n.path); p.setS(key(n, "Host"), n.host);
    p.setI(key(n, "StepS"), n.stepS); p.setI(key(n, "EveryS"), n.everyS); p.setB(key(n, "Aim"), n.aimOnly);
    n.out->configure(n.c);
}

// the port choice, shared by both outputs (the widget ids are made unique by the caller's PushID)
bool portControls(Ns& n) {
    bool ch = false;
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
    return ch;
}
}  // namespace

void sondeNmeaControls() {
    Ns& n = N();
    load(n);
    ImGui::PushID("nmea1");
    bool ch = false;
    ImGui::TextDisabled("%s", tr("NMEA GPS output of the selected sonde"));
    ch |= ImGui::Checkbox(tr("Send the selected sonde as NMEA (GPS sentences)##sondenmea"), &n.c.enabled);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("For a map program that takes a GPS over a serial port. Sends $GPGGA and $GPRMC once a second for the sonde chosen in the list, and nothing while it has no position. Off by default."));
    ch |= portControls(n);
    if (ch) apply(n);
    if (n.c.enabled) ImGui::TextDisabled("%s", n.out->status().c_str());
    ImGui::PopID();

    // the second, independent output: the predicted rest of the flight
    Ns& c = C();
    load(c);
    ImGui::PushID("nmea2");
    ImGui::Separator();
    ch = false;
    ImGui::TextDisabled("%s", tr("Predicted course as a second NMEA output"));
    ch |= ImGui::Checkbox(tr("Send the predicted course of the selected sonde as NMEA"), &c.c.enabled);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("For topo-map software that takes several GPS streams: the predicted way from the sonde to its landing, as GPS positions one after the other, starting over after the landing. Off by default."));
    int aim = c.aimOnly ? 1 : 0;
    ch |= ImGui::RadioButton(tr("Whole course to the landing"), &aim, 0); ImGui::SameLine();
    ch |= ImGui::RadioButton(tr("Aim point (the landing) only"), &aim, 1);
    c.aimOnly = aim == 1;
    ImGui::SetNextItemWidth(80 * gUi);
    if (ImGui::InputInt(tr("seconds of flight between points"), &c.stepS, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) { c.stepS = std::max(1, std::min(3600, c.stepS)); ch = true; }
    ImGui::SetNextItemWidth(80 * gUi);
    if (ImGui::InputInt(tr("seconds between two sentences"), &c.everyS, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) { c.everyS = std::max(1, std::min(3600, c.everyS)); ch = true; }
    ch |= portControls(c);
    if (ch) { apply(c); c.seq.restart(); c.builtT = -1e9; }
    if (c.c.enabled) ImGui::TextDisabled("%s", c.out->status().c_str());
    ImGui::PopID();
}

// every frame from the sonde screens' tick: once a second the sentences of the selected sonde; nothing for no sonde, no position or a sonde
// that has not sent a new frame for 15 s. The predicted course (second output): one position of it every `everyS` seconds, the course
// rebuilt every 5 s, starting over after the landing.
void sondeNmeaTick(const dect2::SondeInfo* sel) {
    Ns& n = N();
    load(n);
    Ns& c = C();
    load(c);
    const double now = steady();
    if (!sel) return;
    if (sel->serial != n.lastSerial || sel->frame != n.lastFrame || sel->lat != n.lastLat || sel->altM != n.lastAlt) {
        n.lastSerial = sel->serial; n.lastFrame = sel->frame; n.lastLat = sel->lat; n.lastAlt = sel->altM; n.lastChangeT = now;
    }
    if (n.c.enabled && now - n.lastChangeT <= 15 && now - n.lastSendT >= 1.0) {
        const std::string t = dect2::sondenmea::sentences(*sel, (double)std::time(nullptr), 1e9);
        if (!t.empty()) { n.lastSendT = now; n.out->send(t); }
    }
    if (!c.c.enabled || now - n.lastChangeT > 15 || !dect2::sondenmea::usable(*sel)) return;
    if (c.builtSerial != sel->serial) { c.seq.set({}); c.seq.restart(); c.builtT = -1e9; c.builtSerial = sel->serial; }
    if (now - c.builtT >= 5) {
        c.builtT = now;
        std::vector<dect2::sondecourse::Point> course;
        if (sondePredictedCourse(*sel, course)) {
            const double wall = (double)std::time(nullptr);
            if (c.aimOnly) { const std::string a = dect2::sondecourse::aimSentences(course.back(), wall); c.seq.set(a.empty() ? std::vector<std::string>() : std::vector<std::string>{a}); }
            else c.seq.set(dect2::sondecourse::sentencesFor(dect2::sondecourse::resample(course, (double)c.stepS), wall));
        } else c.seq.set({});
    }
    if (now - c.lastSendT < c.everyS) return;
    const std::string t = c.seq.next();
    if (t.empty()) return;
    c.lastSendT = now;
    c.out->send(t);
}
