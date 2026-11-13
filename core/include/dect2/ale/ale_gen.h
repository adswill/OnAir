// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF ALE test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// Calls one after another on one channel, each with the answers it gets: a sounding, an individual call (answered, then
// acknowledged), a net call (two members answer in turn), a group call, an AMD message, an allcall. 8-FSK at 125 baud, continuous
// phase, on the upper sideband of the dial; white noise, an SNR in 3 kHz, a tuning error, and bits turned over before the tones at
// a given rate (the channel's bit errors, which the vote and Golay put right).
//
// SynthConfig (makeAleSynth):
//   snrDb       less 10 dB: the SNR in 3 kHz (the default 30 gives 20 dB)
//   cfoHz       the radio's tuning error
//   modeOpt[0]  0 the test calls (aleTestCalls), 1 only the sounding
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct AleGenCall {
    enum Kind { Sounding, Individual, Net, Group, Amd, AllCall };
    Kind kind = Sounding;
    std::string from = "LY2ABC", to = "OH2XYZ";
    std::vector<std::string> members;   // Net: the members that answer; Group: the addresses called (THRU)
    std::string text;                   // Amd: the message (ASCII-64)
    bool tws = true;                    // Sounding: TWS, else TIS
    int scanWords = 6;                  // the scanning part: the called address this many times
    int reps = 4;                       // Sounding: the address this many times
    bool answer = false;                // Individual / Amd: the called station answers and the caller acknowledges
    bool lqa = false;                   // an LQA report in the message part
};

struct AleGenConfig {
    double rate = 250000;
    double dialOffsetHz = 0;            // where the dial sits in the output
    double cfoHz = 0;
    double noiseSigma = 0.02;
    double snrDb = 10;                  // in 3 kHz
    double bitErrorRate = 0;
    uint32_t seed = 1;
    double leadSec = 0.6, gapSec = 1.2; // silence before the first transmission and between two
    bool repeat = false;                // start over after the last call (with 3 s of silence)
    std::vector<AleGenCall> calls;
};

class AleGenerator {
public:
    explicit AleGenerator(const AleGenConfig& c);
    ~AleGenerator();
    void generate(cf32* out, size_t n);
    double durationSec() const;         // to the end of the last transmission
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::vector<AleGenCall> aleTestCalls();
std::unique_ptr<ModeSynth> makeAleSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
