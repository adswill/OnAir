// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal workbench: synthetic test signals with known bits (the tests, wbtool gen, the "demo signal" of the screen).
#pragma once
#include "wb_demod.h"

namespace dect2 {
namespace wb {

struct GenSpec {
    int kind = KFsk2;              // Kind (wb_demod.h): the modulation
    double rate = 48000;           // sample rate
    double baud = 1200;
    size_t nbits = 2000;
    double offsetHz = 0;           // carrier offset from the centre (the signal's DC)
    double snrDb = 40;             // in the signal's own bandwidth ~ in the whole sample rate band (white noise of that power)
    double devHz = 0;              // FSK deviation (0: a sensible value: 2-FSK baud / 2 * ..., see the generator), FM peak deviation
    double rolloff = 0.35;         // PSK pulse: root raised cosine
    bool differential = false;     // PSK: differentially encoded
    double markHz = 1200, spaceHz = 2200;   // AFSK tones
    bool afskOnFm = false;         // AFSK: an FM carrier modulated with the tones instead of the audio itself
    bool realOut = false;          // the output is a real signal (imag 0): AFSK audio, AM
    double toneHz = 1000;          // AM / FM modulating tone
    double depth = 0.8;            // AM depth
    uint32_t seed = 1;
    double leadSec = 0, tailSec = 0;   // noise only before / after the signal
    double gauss = 0.5;            // GMSK bandwidth-time product
};

struct GenResult {
    std::vector<cf32> x;
    Bits bits;                     // what was sent
    double rate = 0;
    bool real = false;
    uint64_t signalStart = 0, signalEnd = 0;   // samples of the signal proper
};

GenResult generate(const GenSpec& g);

// Gaussian noise of the given power (linear, complex: each part has half) added to x
void addNoise(std::vector<cf32>& x, double power, uint32_t seed);

} // namespace wb
} // namespace dect2
