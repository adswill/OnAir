// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NDB test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// A carrier at dialOffsetHz (the engine's tune offset puts it on the carrier after the receiver mixes), amplitude-modulated
// by a keyed tone, or the carrier itself keyed. The ident repeats in Morse at the given speed (PARIS, the same 1200/WPM the
// CW decoder uses). Default is the letters NDB at 7 WPM with a 400 Hz tone, which is the long-wave case this mode is for.
//
// makeNdbSynth (SynthConfig): snrDb = carrier to noise in 1 kHz; cfoHz = extra carrier offset; modeOpt[0] = 0 the 400 Hz tone,
// 1 the 1020 Hz tone, 2 the keyed carrier.
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <string>

namespace dect2 {

struct NdbGenConfig {
    double rate = 256000;
    double snrDb = 30;             // carrier to noise in 1 kHz; >= 90 leaves the noise off
    double cfoHz = 0;
    double dialOffsetHz = 0;      // where the carrier sits in the capture (makeNdbSynth uses -ndbTuning().tuneOffsetHz)
    double toneHz = 400;          // 0: keyed carrier, no modulating tone
    double mod = 0.8;              // AM depth of the tone (kept below 1 so the envelope does not fold)
    double wpm = 7;
    std::string ident = "NDB";
    float amp = 0.25f;
    uint32_t seed = 1;
};

class NdbGenerator {
public:
    explicit NdbGenerator(const NdbGenConfig& c);
    ~NdbGenerator();
    void generate(cf32* out, size_t n);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeNdbSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
