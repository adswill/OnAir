// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ham JS8 (JS8Call's protocol, from its public description and the repo's js8_proto.h): messages to frames, frames to audio in their slots, and a
// small sender that keeps a message's frames in consecutive slots. Normal (15 s), fast (10 s) and turbo (6 s) submodes: 8-FSK, 79 symbols,
// tone spacing = baud (6.25 / 10 / 20 Hz), continuous phase, 10 ms ramps; the burst starts startSec (0.5 / 0.2 / 0.1 s) into the slot.
// Nothing here transmits: js8Send() goes through HamSlotTx::sendAudio (licence, Arm, amateur band) plus the duty-cycle limit.
#pragma once
#include "dect2/ham/ham.h"
#include "dect2/ham/ham_kbd.h"
#include "dect2/js8/js8_proto.h"
#include <deque>
#include <string>
#include <vector>

namespace dect2::ham {

constexpr int kJs8MaxFrames = 24;   // one message never longer than this many slots

// The frames of a message (false and err when it cannot be sent: a bad call, a grid, too long):
bool js8Heartbeat(const std::string& call, const std::string& grid, std::vector<js8::TxFrame>& out, std::string& err);
bool js8Cq(const std::string& call, const std::string& grid, std::vector<js8::TxFrame>& out, std::string& err);
// "FROM: TO TEXT" (directed free text, cmd " "), or a command (" SNR?", " GRID?", " SNR" with num = the report, ">" relay ...) without text
bool js8Directed(const std::string& from, const std::string& to, const std::string& cmd, int num, const std::string& text,
                 std::vector<js8::TxFrame>& out, std::string& err);

// One frame as 48 kHz audio, from its first symbol (the caller starts it startSec into the slot). hz: audio frequency of tone 0.
std::vector<float> js8FrameAudio(const js8::TxFrame& f, int submode, double hz, double rate = kKbdRate);
double js8BwHz(int submode);         // 8 tones: 8 x the spacing

// The frames of the queued messages go out one per slot, back to back; a new message waits for the one before.
class Js8Sender {
public:
    int submode = js8::kNormal;
    double hz = 1500;
    void queue(const std::vector<js8::TxFrame>& frames) { for (const auto& f : frames) q_.push_back(f); }
    void clear() { q_.clear(); }
    size_t pending() const { return q_.size(); }
    // The frame for the slot starting at slotUtc: true and the frame, or false when nothing is queued. Marks it as taken.
    bool frameForSlot(double slotUtc, js8::TxFrame& out);
    double txStartUtc(double slotUtc) const { return slotUtc + js8::spec(submode).startSec; }
private:
    std::deque<js8::TxFrame> q_;
};

// One frame through the transmitter: the same gate, duty limit and PTT handling as the keyboard modes.
bool js8Send(HamSlotTx& tx, TxDuty& duty, const HamTxCheck& gate, const js8::TxFrame& f, int submode, double hz, double level, double nowSec, std::string& err);

}  // namespace dect2::ham
