// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CW (Morse) test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// Several keyed carriers in white noise, each with its own frequency, speed, Farnsworth spacing, hand-keying jitter, fading (QSB) and
// text, repeated with a pause. The key is shaped with 4 ms raised-cosine edges (no clicks).
//
// SynthConfig (makeCwSynth):
//   snrDb       of the strongest signal, in 500 Hz (the others are weaker by their own amount)
//   cfoHz       the radio's tuning error (every signal moves by it)
//   modeOpt[0]  0 the test band (cwTestSignals), 1 only its first signal
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct CwGenSignal {
    double offsetHz = 0;         // from the dial frequency
    double wpm = 20;             // character speed
    double farnsworthWpm = 0;    // overall speed with Farnsworth spacing (0: none)
    std::string text = "CQ CQ DE LY2ABC LY2ABC K";   // "<AR>" style prosigns
    double snrDb = 20;           // key-down carrier to noise in 500 Hz
    double jitter = 0;           // hand keying: each element and gap off by this fraction of its length (standard deviation)
    double dashRatio = 3;        // dash / dot (3 by the book; hand keying is often heavier)
    double qsbDepthDb = 0;       // fading: the level swings this far down ...
    double qsbPeriodSec = 0;     // ... over this period
    double driftHzPerMin = 0;    // a drifting transmitter
    double startSec = 0.3;       // first key-down
    double pauseSec = 2;         // between repeats
    bool repeat = true;
};

struct CwGenConfig {
    double rate = 2e6;
    double dialOffsetHz = 0;     // where the dial frequency sits in the samples (-tuneOffsetHz for the radio, 0 for a recording)
    double cfoHz = 0;
    double noiseSigma = 0.02;    // per component; <= 0: no noise
    uint32_t seed = 1;
    std::vector<CwGenSignal> signals;
};

class CwGenerator {
public:
    explicit CwGenerator(const CwGenConfig& c);
    ~CwGenerator();
    void generate(cf32* out, size_t n);
    double timeSec() const;
    static double messageSec(const CwGenSignal& s);   // one sending of the text, without the pause
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// key-down / key-up durations (seconds, first is a key-down) of a text at a speed, before jitter
std::vector<double> cwKeying(const std::string& text, double wpm, double farnsworthWpm = 0, double dashRatio = 3);
std::vector<CwGenSignal> cwTestSignals();

std::unique_ptr<ModeSynth> makeCwSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
