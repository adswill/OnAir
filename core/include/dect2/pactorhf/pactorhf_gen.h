// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// PACTOR-II / -III / -IV test signals (physical layer only). Nothing is transmitted: the samples only exist in memory or in a file.
//
// The real channel coding is proprietary, so the symbols here are NOT PACTOR frames: every frame is a fixed 40 symbol sync pattern followed by
// pseudo-random symbols, repeated, so that the clock, the carriers, the phase steps and the frame finder can be tested with the exact right answer.
// PACTOR-II: two D-PSK carriers 200 Hz apart, 100 baud (raised-cosine pulses). PACTOR-III: carriers at 480 + 120 n, 100 baud, the carrier sets of
// the six speed levels. PACTOR-IV: one carrier, 1800 baud, root-raised-cosine pulses, PSK / QAM (levels 5 .. 10).
//
// SynthConfig (makePactorHfSynth): snrDb is the signal to noise ratio in 2.5 kHz, cfoHz the radio's tuning error,
// modeOpt[0]: PACTOR-III speed level 1 .. 6 (default 3) or PACTOR-IV constellation 2 / 4 / 8 / 16 (default 4), modeOpt[1]: 1 = a two-path channel.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/engine/mode_synth.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct PactorHfGenConfig {
    int variant = 2;            // 2, 3, 4
    int level = 3;              // PACTOR-III speed level
    int order = 4;              // phase steps per symbol (PACTOR-II / III), constellation points (PACTOR-IV)
    double rate = 8000;         // output sample rate
    bool analytic = true;       // complex output (upper sideband); false: the real part, for the 8 kHz audio tests
    double dialOffsetHz = 0;    // where the dial sits in the samples (the audio is this far above it)
    double audioCentreHz = 885; // PACTOR-II: middle of the pair
    double cfoHz = 0;
    double clockPpm = 0;
    double snrDb = 99;          // in 2.5 kHz; >= 99: no noise
    double amp = 0.2;           // per carrier amplitude (PACTOR-IV: the carrier)
    int frameSyms = 362, syncSyms = 40;
    int twoPath = 0;            // 1: a second path 1.5 ms later at -6 dB
    double fadeHz = 0;          // > 0: flat Rayleigh-like fading with this Doppler spread
    uint32_t seed = 1;
};

class PactorHfGenerator {
public:
    explicit PactorHfGenerator(const PactorHfGenConfig& c);
    ~PactorHfGenerator();
    void generate(cf32* out, size_t n);
    // what is sent: the symbols (phase step index) of every carrier, symbol 0 first (PACTOR-II / III); for the tests
    const std::vector<std::vector<uint8_t>>& sent() const;
    const std::vector<double>& carrierHz() const;   // audio frequencies of the carriers
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Carriers of a PACTOR-III speed level (indices n of 480 + 120 n Hz), the set of Table 3 of the SCS description where it is spelled out
std::vector<int> pactor3Carriers(int level);

std::unique_ptr<ModeSynth> makePactorHfSynth(int variant, const SynthConfig& cfg, double sampleRate);

} // namespace dect2
