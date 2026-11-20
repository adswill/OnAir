// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MIDI controller input (issue #543): the parser and the knob-to-gain mapping of app/midi/midi_logic.h.
#include "../../app/midi/midi_logic.h"
#include <cstdio>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("FAIL: %s\n", msg); fails++; } } while (0)

int main() {
    using namespace midi;
    Parser p; std::vector<Msg> o;
    const uint8_t a[] = {0xB0, 21, 64, 22, 127, 0xF8, 23, 0};   // a controller, running status, a clock byte inside, a third controller
    p.feed(a, sizeof a, o);
    CHECK(o.size() == 3, "three control changes from running status");
    CHECK(o.size() == 3 && o[1].number == 22 && o[1].value == 127 && o[2].number == 23 && o[2].value == 0, "values");
    o.clear();
    const uint8_t b[] = {0xB1, 21};   // split across chunks
    const uint8_t c[] = {99};
    p.feed(b, sizeof b, o); CHECK(o.empty(), "half a message gives nothing");
    p.feed(c, sizeof c, o); CHECK(o.size() == 1 && o[0].channel == 1 && o[0].value == 99, "the second chunk completes it");
    o.clear();
    const uint8_t s[] = {0xF0, 0x7E, 0x00, 0xF7, 0x90, 60, 0, 0x90, 61, 100, 0xE0, 0x00, 0x40};
    p.feed(s, sizeof s, o);
    CHECK(o.size() == 3 && o[0].type == Msg::NoteOff && o[1].type == Msg::NoteOn && o[2].type == Msg::PitchBend && o[2].value == 8192, "sysex skipped, velocity 0 is a note off, pitch bend centre");

    Mapping m; m.cc[Lna] = 21; m.cc[Vga] = 22;
    Msg k; k.type = Msg::ControlChange; k.number = 22; k.value = 127;
    int t = -1; double pos = 0;
    CHECK(route(m, k, t, pos) && t == Vga && pos == 1.0, "CC 22 turns the VGA");
    k.number = 40; CHECK(!route(m, k, t, pos), "an unmapped controller is ignored");
    m.channel = 2; k.number = 21; k.channel = 0; CHECK(!route(m, k, t, pos), "another channel is ignored");
    k.channel = 1; CHECK(route(m, k, t, pos) && t == Lna, "the chosen channel is heard");
    CHECK(stepped(0.0, 0, 40, 8) == 0 && stepped(1.0, 0, 40, 8) == 40 && stepped(0.5, 0, 40, 8) == 24, "LNA steps of 8 dB");
    CHECK(stepped(1.0, 0, 62, 2) == 62 && stepped(0.33, 0, 62, 2) % 2 == 0, "VGA steps of 2 dB");
    std::printf(fails ? "midi FAILED\n" : "midi ok\n");
    return fails ? 1 : 0;
}
