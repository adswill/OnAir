// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Fleet Broadcast test signal. Nothing is transmitted: the samples only exist in memory or in a file.
// SIMULATED: differential BPSK at 1200 Bd carrying 16 channels of one bit per 16-bit frame, built from the public description only. The real sync
// pattern of channel 0 is not published: the generator uses a 31-bit m-sequence of its own (x^5 + x^3 + 1), so only the modem, the frame finder
// and the channel classifier can be tested with it. Pulse edges are half-cosine over a quarter symbol either side of each boundary.
//
// SynthConfig (makeFbrSynth): snrDb (symbol power over noise power in 1.2 kHz); modeOpt[0] scenario: 0 mixed (ch0 sync, ch1 and ch4 reversals,
// ch2 idle 1, ch3 idle 0, the rest random), 1 sync and idle only, 2 noise only, 3 a plain unmodulated carrier, 4 random on every channel (no sync);
// modeOpt[1]: 1 inverted polarity; modeVal[0]: offset of the carrier from the radio's centre, Hz (0: 1500); modeVal[1]: symbol clock error, ppm;
// modeVal[2]: fade depth, dB (slow flat fading at 0.5 Hz); modeVal[3]: Doppler drift, Hz per second.
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>

namespace dect2 {

struct FbrGenConfig {
    double rate = 250e3;
    double snrDb = 20;
    int scenario = 0;
    bool invert = false;
    double offsetHz = 1500;
    double clockPpm = 0;
    double fadeDepthDb = 0;
    double dopplerHzPerS = 0;
    uint32_t seed = 1;
};

class FbrGenerator {
public:
    explicit FbrGenerator(const FbrGenConfig& c);
    ~FbrGenerator();
    void generate(cf32* out, size_t n);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The bit a scenario sends on a channel in a frame (before differential coding and polarity), so a test can compare the result
int fbrTestBit(int scenario, int channel, uint64_t frame);
constexpr int kFbrTestSyncPeriod = 31;

std::unique_ptr<ModeSynth> makeFbrSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
