// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Test signals of the wxsat links (wxsat_tel.h). Nothing is transmitted: the samples only exist in memory or in a file.
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/wxsat/wxsat_tel.h"
#include <cstdint>
#include <memory>

namespace dect2 {

struct WxsatGenConfig {
    int profile = 0;
    double rate = 6e6;
    double snrDb = 30;           // Es/N0 in the symbol (chip) rate; APT: carrier to noise in 40 kHz; >= 99: none
    double cfoHz = 0;
    double driftHzPerS = 0;      // Doppler rate
    double clockPpm = 0;
    double phaseDeg = 0;
    double pmIndexDeg = 67.5;    // Manchester links: the phase swing, as on the real downlink
    double amp = 0.4;
    double fileSymbolRate = 0;   // HimawariCast, GeoNetCast: another symbol rate than the link's (tests of the 30 Msym/s chain at a low rate)
    int lines = 400;             // the test picture repeats after this many lines
    uint32_t seed = 1;
};

class WxsatGenerator {
public:
    explicit WxsatGenerator(const WxsatGenConfig& c);
    ~WxsatGenerator();
    void generate(cf32* out, size_t n);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeWxsatSynth(const SynthConfig& cfg, double sampleRate, int profile);

} // namespace dect2
