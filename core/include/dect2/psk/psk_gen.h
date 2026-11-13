// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// PSK31 / PSK63 test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// Several PSK signals in white noise, each with its own audio frequency, modulation, text, start, drift and clock error, sent with
// the raised-cosine symbol shaping of PSK31 (BPSK: a reversal is a 0, no change a 1; QPSK: fldigi's phase mapping of the K = 5 code),
// an idle preamble of reversals, the text, then idle again.
//
// SynthConfig (makePskSynth):
//   snrDb       of the strongest signal, in its own bandwidth (the others are weaker by their own amount)
//   cfoHz       the radio's tuning error (every signal moves by it)
//   modeOpt[0]  0 the test band (pskTestSignals), 1 only its first signal
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct PskGenSignal {
    double offsetHz = 1000;      // audio frequency: from the dial frequency
    int mode = 0;                // kPskModes
    std::string text = "CQ CQ DE LY2ABC LY2ABC PSE K\n";
    double snrDb = 20;           // in the signal's bandwidth (the baud rate)
    double startSec = 0.3;
    double idleSec = 1.0;        // reversals before (and after) the text
    double pauseSec = 2;         // carrier off between repeats
    bool repeat = true;
    double driftHzPerMin = 0;
    double clockPpm = 0;         // the sender's symbol clock off by this much
    double phase0 = 0;
};

struct PskGenConfig {
    double rate = 2e6;
    double dialOffsetHz = 0;     // where the dial frequency sits in the samples (-tuneOffsetHz for the radio, 0 for a recording)
    double cfoHz = 0;
    double noiseSigma = 0.02;    // per component; <= 0: no noise
    uint32_t seed = 1;
    std::vector<PskGenSignal> signals;
};

class PskGenerator {
public:
    explicit PskGenerator(const PskGenConfig& c);
    ~PskGenerator();
    void generate(cf32* out, size_t n);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::vector<PskGenSignal> pskTestSignals();
std::unique_ptr<ModeSynth> makePskSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
