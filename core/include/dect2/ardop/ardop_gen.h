// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ARDOP test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// A script of ARDOP frames (leader of two tones, the sync symbol with its phase reversed, the ten frame type symbols, the body of 4FSK
// frames at 50 or 100 baud) as the upper sideband audio of a radio, in white noise, repeated with a pause. PSK, QAM and 600 baud frames
// are not modulated: giving one of those types sends its header only (the receiver then reports the header).
//
// SynthConfig (makeArdopSynth):
//   snrDb       added to the tone SNR of the test signal (given in 2.5 kHz)
//   cfoHz       the radio's tuning error
//   modeOpt[0]  0 the full script, 1 only the ID frame
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct ArdopGenFrame {
    int code = 0x30;
    uint8_t sid = 0xFF;
    std::vector<uint8_t> body;   // what follows the two type bytes (ardop_proto.h make...)
    int leaderMs = 200;          // 100 .. 1000 (a multiple of 20)
    double gapSec = 0.5;         // silence after the frame
};

struct ArdopGenConfig {
    double rate = 2e6;
    double dialOffsetHz = 0;     // where the dial frequency sits in the samples (-tuneOffsetHz for the radio, 0 for a recording)
    double cfoHz = 0;            // tuning error
    double snrDb = 20;           // tone SNR in 2.5 kHz; >= 99: no noise
    double noiseSigma = 0.05;    // per component, when there is noise
    double clockPpm = 0;         // the sender's clock off by this much
    double amp = 0.4;            // tone amplitude when there is no noise
    bool repeat = true;
    double pauseSec = 2.0;       // between repeats
    double startSec = 0.3;
    std::vector<ArdopGenFrame> frames;
    uint32_t seed = 1;
};

class ArdopGenerator {
public:
    explicit ArdopGenerator(const ArdopGenConfig& c);
    ~ArdopGenerator();
    void generate(cf32* out, size_t n);
    double scriptSeconds() const;   // one pass of the script
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::vector<ArdopGenFrame> ardopTestFrames();
std::unique_ptr<ModeSynth> makeArdopSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
