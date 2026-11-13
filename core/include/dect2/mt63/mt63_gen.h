// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MT63 test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// MT63 signals in white noise, each with its own bandwidth (500 / 1000 / 2000 Hz), interleave, audio centre frequency, text, level,
// start, drift and symbol clock error, optionally through a fading and echo channel. The text is sent as 7-bit characters (idle = NUL)
// between idle stretches, then repeated. The signal is built at 16 kHz and interpolated to the output rate, then moved by the dial offset.
//
// SynthConfig (makeMt63Synth):
//   snrDb       added to the level of every signal (they are given in the signal bandwidth)
//   cfoHz       the radio's tuning error
//   modeOpt[0]  0 the test band (mt63TestSignals), 1 only its first signal
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct Mt63GenSignal {
    int bandwidth = 1000;        // Hz: 500, 1000, 2000
    bool longInterleave = false;
    double centreHz = 1500;      // audio frequency of the centre of the 64 carriers, from the dial
    std::string text = "CQ CQ CQ DE LY2ABC LY2ABC PSE K\n";
    double snrDb = 10;           // in the signal bandwidth
    double startSec = 0.5;       // idle characters before the first text
    double pauseSec = 2.0;       // idle between repeats
    bool repeat = true;
    double driftHzPerMin = 0;
    double clockPpm = 0;         // the sender's symbol clock off by this much
    double fadeDepth = 0;        // 0 none .. 1: the amplitude swings between 1 and 1 - depth
    double fadeHz = 0.3;         // fading rate
    double echoMs = 0;           // a second path this long behind the first (0 none)
    double echoDb = -6;
    uint32_t seed = 1;
};

struct Mt63GenConfig {
    double rate = 2e6;
    double dialOffsetHz = 0;     // where the dial frequency sits in the samples (-tuneOffsetHz for the radio, 0 for a recording)
    double cfoHz = 0;
    double noiseSigma = 0.02;    // per component; <= 0: no noise
    uint32_t seed = 1;
    std::vector<Mt63GenSignal> signals;
};

class Mt63Generator {
public:
    explicit Mt63Generator(const Mt63GenConfig& c);
    ~Mt63Generator();
    void generate(cf32* out, size_t n);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::vector<Mt63GenSignal> mt63TestSignals();
std::unique_ptr<ModeSynth> makeMt63Synth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
