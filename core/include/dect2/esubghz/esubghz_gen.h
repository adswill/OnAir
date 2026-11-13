// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Flipper Zero Sub-GHz chat: the GFSK modulator (CC1101 9.99 kbaud, 19.04 kHz deviation, Gaussian BT 0.5) that both the transmitter and the
// test signal use. The test signal sends nothing: its samples only exist in memory or in a file.
// Test signal (makeEsubghzSynth): a short conversation of two Flippers, plain lines and lines encrypted with the password "onair" (one of them
// longer than one packet), with silence between, over complex white noise across the whole sample rate. SynthConfig: snrDb (carrier to noise
// in 100 kHz), cfoHz. The signal sits at -tuneOffsetHz from the centre, where a radio tuned for this mode sees it.
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/esubghz/esubghz_proto.h"
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

class TxGenerator;
struct TxRequest;

// IQ of one packet's octets (packetOctets()) at rate, offsetHz from the centre, amplitude amp, appended to out: one sample per 1/rate for every bit, then
// one more symbol while the carrier ramps down (it ramps up over the first). Phase-continuous across calls through phase.
void esubghzModulate(const std::vector<uint8_t>& octets, double rate, double offsetHz, float amp, std::vector<cf32>& out, double& phase);
// A whole message as a Flipper sends it: the packets of splitMessage() with kChunkGapSec of silence between, leadSec of silence before and tailSec after.
std::vector<cf32> esubghzMessageIq(const std::vector<uint8_t>& msg, double rate, double offsetHz, float amp, double leadSec = 0.002, double tailSec = 0.005);

struct EsgLine { std::string line; bool encrypted = false; double gapSec = 0.8; };
std::vector<EsgLine> esubghzTestScript();
constexpr const char* kEsubghzTestPassword = "onair";

// The transmitter (TX row "esubghz"). r.payload, when set, is the message as it goes on the air (the chat screen encrypts it); otherwise r.text is
// sent as a plain line (with "\r\n"). Opt "esubghz.ifHz": the signal is made this far from DC (the chat screen tunes the radio the other way, so the
// radio's own carrier leak stays off the channel). The generator ends after the message.
std::unique_ptr<TxGenerator> makeEsubghzTxGenerator(const TxRequest& r, double rateHz, std::string& err);

std::unique_ptr<ModeSynth> makeEsubghzSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
