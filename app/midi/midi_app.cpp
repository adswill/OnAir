// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MIDI knobs for the radio gain (issue #543): a controller's knob moves the HackRF LNA or VGA, or the overall gain of a radio that has one.
// Settings > Radio > MIDI controller: on/off, the port (part of its name; empty: the first), the MIDI channel (0: any) and the controller number
// of each knob (-1: none), and the number of the antenna power button (a controller or a note). The button's light follows the antenna power. Touching a knob turns Auto gain off, as moving the slider does.
#include "../app.h"
#include "midi_in.h"
#include <cmath>

namespace {

struct State {
    midi::Input in;
    midi::Output out;         // the same controller's output, to light the antenna power button
    int led = -1;             // the light last sent (0/1), -1: none yet
    std::string wantedPort;   // what the settings asked for, to notice a change
    bool wasOn = false;
    double nextCheck = 0;
    std::string reported;     // the last error told in the log
};

State& S() { static State s; return s; }

midi::Mapping mapping() {
    plat::Prefs& d = plat::prefs();
    midi::Mapping m;
    m.cc[midi::Lna] = (int)d.getI("midiCcLna", -1);
    m.cc[midi::Vga] = (int)d.getI("midiCcVga", -1);
    m.cc[midi::Gain] = (int)d.getI("midiCcGain", -1);
    m.channel = (int)d.getI("midiChannel", 0);
    m.bias = (int)d.getI("midiCcBias", -1);
    m.biasNote = d.getB("midiBiasNote", false);
    return m;
}

// the port whose name holds the wanted text (empty text: the first port)
bool pickPort(const std::string& want, midi::PortInfo& out) {
    const std::vector<midi::PortInfo> ports = midi::listPorts();
    for (const midi::PortInfo& p : ports) {
        if (want.empty() || p.name.find(want) != std::string::npos) { out = p; return true; }
    }
    return false;
}

} // namespace

void midiTick(App& a) {
    State& s = S();
    plat::Prefs& d = plat::prefs();
    const bool on = d.getB("midiOn", false);
    const double now = ImGui::GetTime();
    if (!on) {
        if (s.wasOn) { s.in.close(); s.out.close(); s.led = -1; s.wasOn = false; s.wantedPort.clear(); }
        return;
    }
    // every second: the port may have been plugged in, unplugged or changed in the settings
    if (!s.wasOn || now >= s.nextCheck) {
        s.nextCheck = now + 1.0;
        const std::string want = d.getS("midiPort", "");
        if (!s.in.isOpen() || want != s.wantedPort) {
            s.in.close(); s.out.close(); s.led = -1;
            s.wantedPort = want;
            midi::PortInfo p;
            if (pickPort(want, p)) {
                if (s.in.open(p)) { a.engine.log("MIDI controller: " + p.name); s.reported.clear(); s.out.open(p); }
                else if (s.in.error() != s.reported) { s.reported = s.in.error(); a.engine.log("MIDI controller: " + s.reported); }
            }
        }
        s.wasOn = true;
    }
    if (!s.in.isOpen()) return;
    std::vector<midi::Msg> msgs;
    s.in.poll(msgs);
    const midi::Mapping m = mapping();
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) return;
    const DeviceInfo& dev = a.devices[(size_t)a.devIdx];
    bool changed = false, biasChanged = false;
    for (const midi::Msg& msg : msgs) {
        if (dev.hasBiasTee) {
            const int r = midi::biasRequest(m, msg, a.tune.biasTee);
            if (r >= 0) { if ((r == 1) != a.tune.biasTee) { a.tune.biasTee = r == 1; biasChanged = true; } continue; }
        }
        int t = 0; double pos = 0;
        if (!midi::route(m, msg, t, pos)) continue;
        if (t == midi::Lna && dev.kind == DeviceInfo::HackRF) {
            const int v = midi::stepped(pos, 0, 40, 8);
            if (v != a.tune.lnaDb) { a.tune.lnaDb = v; changed = true; }
        } else if (t == midi::Vga && dev.kind == DeviceInfo::HackRF) {
            const int v = midi::stepped(pos, 0, 62, 2);
            if (v != a.tune.vgaDb) { a.tune.vgaDb = v; changed = true; }
        } else if (t == midi::Gain && dev.isGeneric()) {
            const double hi = std::max(dev.gainMaxDb, dev.gainMinDb + 1.0);
            const double v = std::round(dev.gainMinDb + pos * (hi - dev.gainMinDb));
            if (v != a.tune.gainDb) { a.tune.gainDb = v; changed = true; }
        }
    }
    if (changed || biasChanged) {
        if (changed) a.agcOn = false;
        if (biasChanged) saveBiasTee(a);
        if (a.engine.running()) a.engine.retune(a.tune);
    }
    // the light follows the setting, whether it was changed here or in the toolbar (and is sent again after the port opens)
    if (s.out.isOpen() && m.bias >= 0) {
        const int want = dev.hasBiasTee && a.tune.biasTee ? 1 : 0;
        if (want != s.led) { uint8_t b[3]; midi::biasLed(m, want == 1, b); s.out.send(b, 3); s.led = want; }
    }
}
