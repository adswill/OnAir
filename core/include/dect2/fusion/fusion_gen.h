// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Yaesu System Fusion test signal: whole transmissions (header, communication frames, terminator) as bits, and the C4FM modulator.
// Voice sub-frames are pseudo-random bytes (there is no speech codec here); tests compare them with what the receiver hands the plug-in.
#pragma once
#include "dect2/fusion/fusion_proto.h"
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::fusion {

struct GenParams {
    std::string dest = "CQCQCQ", src = "N0CALL", down, up;
    std::string rem[4];            // 5 characters each (radio IDs etc.)
    std::string slow;              // slow data (text): V/D mode 1 up to 100 bytes, V/D mode 2 up to 20, data FR up to 260
    int dt = 0;                    // 0 V/D mode 1, 1 data FR, 2 V/D mode 2, 3 voice FR
    int commFrames = 8;            // communication frames between the header and the terminator
    int cm = 0, mr = 0, voip = 0;
    int sqlOff = 1, sqlCode = 0;
    uint32_t seed = 1;
};

// The bytes of voice sub-frame `sub` of communication frame `frame` (9, 7 or 18 bytes by kind 1, 2, 3)
std::vector<uint8_t> genVoiceBytes(uint32_t seed, int frame, int sub, int kind);

// Header + communication frames + terminator. frames (optional) receives the content of each frame in order.
bool genTransmissionBits(const GenParams& p, std::vector<uint8_t>& bits, std::vector<FrameContent>* frames = nullptr);

// C4FM: dibits (two bits each) -> raised-cosine shaped 4-level FSK at +-900 / +-2700 Hz, as complex baseband at `fs`
// (shift moves it by cfoHz). idleSymbols of silence-level carrier go before and after.
std::vector<std::complex<float>> modulateC4fm(const std::vector<uint8_t>& bits, double fs, double cfoHz = 0, float amp = 1.f, int idleSymbols = 40, double devScale = 1.0, double rateScale = 1.0);   // devScale: deviation error, rateScale: symbol clock error

} // namespace dect2::fusion
