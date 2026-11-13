// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FSQ / IFKP test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// Signals in white noise, each with its own mode, speed, audio frequency, text, level, start, drift and symbol clock error, optionally through a
// fading and echo channel. The text is sent as symbols between silent stretches (the transmitter is off), then repeated. The signal is built at
// 16 kHz and interpolated to the output rate, then moved by the dial offset.
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct Kbd33GenSignal {
    bool ifkp = false;
    int speed = 2;               // FSQ 0..4 (1.5 .. 6 baud), IFKP 0..2 (0.5 .. 2.0)
    double centreHz = 1500;      // audio frequency of the middle of the 33 tones, from the dial
    std::string text;
    double snrDb = 10;           // in the band of the 33 tones
    double amplitude = 0;        // > 0: this tone amplitude instead of the one snrDb gives (the transmitter's clean signal)
    double startSec = 0.5;       // silence before the first text
    double pauseSec = 2.0;       // silence between repeats
    bool repeat = true;
    double driftHzPerMin = 0;
    double clockPpm = 0;         // the sender's symbol clock off by this much
    double fadeDepth = 0;        // 0 none .. 1: the amplitude swings between 1 and 1 - depth
    double fadeHz = 0.3;
    double echoMs = 0;
    double echoDb = -6;
    int startTone = 5;
    uint32_t seed = 1;
};

struct Kbd33GenConfig {
    double rate = 2e6;
    double dialOffsetHz = 0;     // where the dial frequency sits in the samples (-tuneOffsetHz for the radio, 0 for a recording)
    double cfoHz = 0;
    double noiseSigma = 0.02;    // per component; <= 0: no noise
    uint32_t seed = 1;
    std::vector<Kbd33GenSignal> signals;
};

class Kbd33Generator {
public:
    explicit Kbd33Generator(const Kbd33GenConfig& c);
    ~Kbd33Generator();
    void generate(cf32* out, size_t n);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// An FSQ line the way a station sends it: " \n" + CALL + ":" + CRC of the callsign + body, then the end ("  \b  " for a directed message, "\n " otherwise)
std::string fsqFrame(const std::string& call, const std::string& body, bool directed);

} // namespace dect2
