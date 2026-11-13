// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Test signals for the Smart CubeSat decoder. Nothing is transmitted: the samples only exist in memory. One call makes one satellite's burst train
// (some frames of one framing on one modulation) at any sample rate, with the impairments the real thing has: a carrier offset, a Doppler ramp, a
// transmitter clock error, noise. Add several into one wide capture with mixInto().
#pragma once
#include "cubesat_proto.h"
#include "dect2/common/ring.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace cubesat {

struct SmartSynth {
    int mod = 3;                  // ModKind of cubesat_db.h: 1 CW, 2 AFSK, 3 FSK/GMSK, 4 BPSK
    int baud = 9600;
    int framing = Ax25G3ruh;      // Framing, or kFramingAo40 / kFramingText / kFramingCw (cubesat_telem.h)
    bool scramble = true;         // AX.25: the G3RUH scrambler
    double rate = 96000;          // output sample rate
    double carrierHz = 0;         // the carrier's place in the output band
    double dopplerHz = 0;         // an extra offset at t = 0 ...
    double dopplerRateHzS = 0;    // ... and its change per second
    double clockPpm = 0;          // the transmitter's symbol clock error
    double snrDb = 40;            // carrier power to noise power in 10 kHz
    double devHz = 0;             // FSK deviation (0: a quarter of the baud rate, MSK like)
    double gaussBt = 0.5;
    int frames = 3;
    double leadSec = 0.3, gapSec = 0.4;
    double wpm = 18;              // CW
    std::string callsign = "TSTSAT";
    std::string text = "";        // text beacon / CW message ("" a default)
    uint64_t seed = 1;
};

struct SynthSent {
    int framing = 0;
    std::vector<uint8_t> data;    // as the decoder reports it (AX.25: the frame without FCS; CSP packet; AO-40: the 256 bytes; text)
    std::string text;
};

// The burst train: leadSec of carrier-less noise, then `frames` frames with gapSec of carrier between. Unit carrier amplitude (before noise).
std::vector<cf32> smartSynth(const SmartSynth& s, std::vector<SynthSent>* sent = nullptr);
// out += a * x, x starting at sample `at` (grown when needed)
void mixInto(std::vector<cf32>& out, const std::vector<cf32>& x, size_t at, float a);
// white Gaussian noise of unit complex variance times sigma, added to x
void addNoise(std::vector<cf32>& x, float sigma, uint64_t seed);
// Morse timing of a text as (key down?, seconds) runs, for tests of the decoder
std::vector<std::pair<bool, double>> morseRuns(const std::string& text, double wpm);

} // namespace cubesat
} // namespace dect2
