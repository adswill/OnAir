// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Project Horus test signals (in memory only; nothing is transmitted). SynthConfig options:
//   modeOpt[0]  0 Horus Binary v2 (ID 256, 4FSKTEST-V2), 1 Horus Binary v1 (ID 0), 2 RTTY 100 Bd 7N2, 3 RTTY 300 Bd 8N2, 4 RTTY 50 Bd 7N2,
//               5 three balloons at once: v2 on the dial, v1 12 kHz above, RTTY 100 Bd 18 kHz below (needs 64 kHz of input or more)
//   modeOpt[1]  tone spacing in Hz (0: 270 Hz for 4FSK, 500 Hz for RTTY)
//   modeOpt[2]  1: mirrored spectrum (the tones in reverse order, as an LSB receiver hears them)
//   modeOpt[3]  noise seed (0: 1)
//   modeVal[0]  symbol rate error in ppm
//   snrDb       SNR of each balloon in 3 kHz; cfoHz the frequency error
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/horus/horus_proto.h"
#include <memory>
#include <vector>

namespace dect2::horus {
// Where the generator puts the dial at this sample rate (the receiver's signal offset must match): -tuneOffsetHz at the usual
// radio rates, 0 when the rate is too low to hold the offset (sound-card rates)
double synthDialHz(double rate);
// The bits on air for one binary payload: 8 preamble bytes, the unique word and the coded frame, most significant bit first
std::vector<uint8_t> airBits(const std::vector<uint8_t>& payload);
// The simulated flight: packet number k of a payload (position, altitude, time advance with k)
Packet simPacket(int kind, int k, double secondsPerPacket);
} // namespace dect2::horus

namespace dect2 {
std::unique_ptr<ModeSynth> makeHorusSynth(const SynthConfig& cfg, double rate);
}
