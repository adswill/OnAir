// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Test signals of the X-band links (xband_tel.h): a test picture per instrument, framed and coded as the link has it. Nothing is transmitted: the samples only exist in memory or in a file.
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/xband/xband_tel.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct XbandGenConfig {
    int profile = 0;
    double rate = 25e6;
    double snrDb = 30;           // Es/N0 in the symbol rate; >= 99: none
    double cfoHz = 0;
    double driftHzPerS = 0;
    double clockPpm = 0;
    double phaseDeg = 0;
    double amp = 0.4;
    int ambiguity = 0;           // 0..7: 90 degree steps, Q rail inverted (a down-converter / sign convention the receiver must settle)
    uint32_t seed = 1;
};

class XbandGenerator {
public:
    explicit XbandGenerator(const XbandGenConfig& c);
    ~XbandGenerator();
    void generate(cf32* out, size_t n);
    uint64_t caduCount() const;
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

// The test frames of a profile as CADUs (what a .cadu file holds)
std::vector<uint8_t> xbandTestCadus(int profile, int frames);
uint16_t xbTestPixel(int channel, int x, int y);      // 12-bit test picture

std::unique_ptr<ModeSynth> makeXbandSynth(const SynthConfig& cfg, double sampleRate, int profile);

} // namespace dect2
