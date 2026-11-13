// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HRPT / AHRPT test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// A test picture (five channels, 10 bit, different in each) coded the way hrpt_proto.h has it: MetOp AVHRR lines as source packets of
// APID 103 or 104 in VCDUs, Reed-Solomon, randomised, sync word, CCSDS convolutional code (G2 inverted), QPSK with a root raised cosine
// (0.6); FY-3: the same chain at 2.8 Msym/s carrying filler packets only; NOAA: minor frames of 10-bit words, Manchester, BPSK with a root
// raised cosine (1.0). Over complex white noise.
//
// SynthConfig (makeHrptSynth): snrDb (signal to noise in the symbol rate), cfoHz, modeOpt[0] variant (0 or 1 MetOp, 2 FY-3, 3 NOAA),
// modeOpt[1] 1: channel 3B (APID 104) instead of 3A (APID 103).
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct HrptGenConfig {
    double rate = 8e6;
    int variant = 1;             // 1 MetOp AHRPT, 2 FY-3 AHRPT, 3 NOAA HRPT
    double snrDb = 30;           // in the symbol rate; >= 99: no noise
    double cfoHz = 0;
    double clockPpm = 0;
    double driftHzPerS = 0;      // carrier offset change (Doppler rate)
    double phaseDeg = 0;         // carrier phase at the start
    bool invert = false;         // NOAA: the opposite polarity
    int apid = 103;
    double amp = 0.25;
    int lines = 60;              // lines in the picture, then it starts again
    uint32_t seed = 1;
};

class HrptGenerator {
public:
    explicit HrptGenerator(const HrptGenConfig& c);
    ~HrptGenerator();
    void generate(cf32* out, size_t n);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The test picture's 10-bit pixel of a channel (0 .. 4) at (x, y)
uint16_t hrptTestPixel(int channel, int x, int y);

std::unique_ptr<ModeSynth> makeHrptSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
