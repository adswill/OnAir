// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Robust Packet test signal (physical layer only). Nothing is transmitted: the samples only exist in memory or in a file.
//
// The real channel coding is not public, so the symbols are NOT RPR frames: every burst is a 8-symbol-per-carrier DPSK preamble (160 ms, a fixed
// pseudo-random pattern of our own) followed by pseudo-random differential symbols, then a pause. What follows the published facts: eight carriers
// 60 Hz apart around 1500 Hz, 50 baud, D-BPSK (level 1) or D-QPSK (level 2). The pulse shape is an assumption: a 20 ms symbol made of a 1/60 s active part (60 Hz spacing is then orthogonal) and a 3.33 ms guard, cross-faded.
// SynthConfig: snrDb is the signal to noise ratio in 2.5 kHz, cfoHz the tuning error, modeOpt[0]: speed level 1 or 2 (default 2).
#pragma once
#include "dect2/common/ring.h"
#include "dect2/engine/mode_synth.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct RprGenConfig {
    int level = 2;              // speed level: 1 D-BPSK, 2 D-QPSK
    double rate = 8000;
    bool analytic = true;       // complex output (upper sideband); false: the real part (8 kHz audio tests)
    double dialOffsetHz = 0;
    double audioCentreHz = 1500;
    double cfoHz = 0;
    double clockPpm = 0;
    double snrDb = 99;          // in 2.5 kHz; >= 99: no noise
    double amp = 0.08;          // per carrier amplitude
    int burstSyms = 60;         // payload symbols per carrier (after the 8 symbol preamble)
    double gapSec = 1.5;
    double fadeHz = 0;          // > 0: flat Rayleigh-like fading with this Doppler spread
    uint32_t seed = 1;
};

class RprGenerator {
public:
    explicit RprGenerator(const RprGenConfig& c);
    ~RprGenerator();
    void generate(cf32* out, size_t n);
    // what was sent: the phase step index of every carrier (burst payloads one after the other, the preamble first in each burst)
    const std::vector<std::vector<uint8_t>>& sent() const;
    double burstSec() const;     // length of one burst
    double periodSec() const;    // burst + gap
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeRprSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
