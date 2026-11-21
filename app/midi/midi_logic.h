// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MIDI controller input (issue #543), the pure part: a byte-stream parser (running status, real-time bytes and system exclusive skipped) and the
// mapping of a control-change knob to a radio gain (HackRF LNA, VGA, or the overall gain). No operating system and no App in here, so
// tests/app/test_midi.cpp drives it directly. The ports are in midi_in.h, the use of the messages in midi_app.cpp.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace midi {

struct Msg {
    enum Type { None, NoteOff, NoteOn, ControlChange, PitchBend } type = None;
    int channel = 0;   // 0 .. 15
    int number = 0;    // note or controller number; 0 for a pitch bend
    int value = 0;     // 0 .. 127 (a pitch bend: 0 .. 16383)
};

// Turns the bytes of a MIDI input into messages. Fed in chunks of any size.
class Parser {
public:
    // appends the messages completed by these bytes
    void feed(const uint8_t* p, size_t n, std::vector<Msg>& out) {
        for (size_t i = 0; i < n; i++) byte(p[i], out);
    }
    void reset() { status_ = 0; have_ = 0; inSysex_ = false; }

private:
    static int dataBytes(uint8_t st) {
        switch (st & 0xF0) {
            case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 2;
            case 0xC0: case 0xD0: return 1;
            default: return -1;   // system messages are not used
        }
    }
    void byte(uint8_t b, std::vector<Msg>& out) {
        if (b >= 0xF8) return;                          // real-time (clock, active sensing ...): may sit inside any message
        if (b & 0x80) {
            inSysex_ = b == 0xF0;
            if (b >= 0xF0) { status_ = 0; have_ = 0; return; }   // system common: cancels running status
            status_ = b; have_ = 0;
            return;
        }
        if (inSysex_ || status_ == 0) return;
        data_[have_++] = b;
        const int need = dataBytes(status_);
        if (need < 0) { have_ = 0; return; }
        if (have_ < need) return;
        have_ = 0;
        Msg m; m.channel = status_ & 15;
        switch (status_ & 0xF0) {
            case 0x80: m.type = Msg::NoteOff; m.number = data_[0]; m.value = data_[1]; break;
            case 0x90: m.type = data_[1] ? Msg::NoteOn : Msg::NoteOff; m.number = data_[0]; m.value = data_[1]; break;   // velocity 0 is a note off
            case 0xB0: m.type = Msg::ControlChange; m.number = data_[0]; m.value = data_[1]; break;
            case 0xE0: m.type = Msg::PitchBend; m.value = data_[0] | (data_[1] << 7); break;
            default: return;
        }
        out.push_back(m);
    }
    uint8_t status_ = 0;
    uint8_t data_[2] = {0, 0};
    int have_ = 0;
    bool inSysex_ = false;
};

enum Target { Lna = 0, Vga, Gain, TargetCount };

// Which controller number moves which gain (-1: none), and the one channel listened to (0: any, 1 .. 16)
struct Mapping {
    int cc[TargetCount] = {-1, -1, -1};
    int channel = 0;
};

// The target a message turns, and where the knob stands from 0 to 1; false when the message is not for any gain
inline bool route(const Mapping& m, const Msg& msg, int& target, double& pos) {
    if (msg.type != Msg::ControlChange) return false;
    if (m.channel > 0 && msg.channel != m.channel - 1) return false;
    for (int t = 0; t < TargetCount; t++) {
        if (m.cc[t] >= 0 && m.cc[t] == msg.number) { target = t; pos = std::min(1.0, std::max(0.0, msg.value / 127.0)); return true; }
    }
    return false;
}

// The knob position on a gain range, snapped to the radio's step (HackRF: LNA 0 .. 40 in 8 dB, VGA 0 .. 62 in 2 dB)
inline int stepped(double pos, int lo, int hi, int step) {
    const double v = lo + pos * (hi - lo);
    const int k = (int)std::lround((v - lo) / step);
    return std::min(hi, std::max(lo, lo + k * step));
}

} // namespace midi
