// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meteor-M LRPT test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// A test picture (gradients, a grid and rings, different in each channel) coded the way MSU-MR does it (meteor_proto.h): image
// packets of APID 64 - 69 and a telemetry packet, 43 a period on one counter, into VCDUs, Reed-Solomon, randomised, the sync word,
// convolutional code, then per modulation the differential code and the interleaver with its markers, root raised cosine (0.6) QPSK
// or OQPSK, over complex white noise.
//
// SynthConfig (makeMeteorSynth): snrDb (signal to noise in the symbol rate), cfoHz, modeOpt[0] modulation (0 or 1 OQPSK 72k, 2 QPSK
// 72k, 3 OQPSK 80k interleaved), modeOpt[1] 1: night channels (1, 2, 5: APIDs 64, 65, 68), else day (1, 2, 3).
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct MeteorGenConfig {
    double rate = 1e6;
    int modulation = 1;          // 1 OQPSK 72k, 2 QPSK 72k, 3 OQPSK 80k interleaved
    double snrDb = 30;           // in the symbol rate; >= 99: no noise
    double cfoHz = 0;
    double cfoDriftHzPerSec = 0; // Doppler: the carrier moves by this much every second
    double clockPpm = 0;
    double amp = 0.25;
    std::vector<int> apids = {64, 65, 66};
    int quality = 80;
    int rows = 40;               // rows of 8 lines in the picture, then it starts again
    uint32_t seed = 1;
};

class MeteorGenerator {
public:
    explicit MeteorGenerator(const MeteorGenConfig& c);
    ~MeteorGenerator();
    void generate(cf32* out, size_t n);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The test picture's pixel of a channel (0 .. 5) at (x, y)
uint8_t meteorTestPixel(int channel, int x, int y);

std::unique_ptr<ModeSynth> makeMeteorSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
