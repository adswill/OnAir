// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// BGAN forward-bearer test signal: a real frame structure (UW, pilots, scrambled and turbo coded FEC blocks with synthetic signalling). Nothing is transmitted.
// SynthConfig for the mode's test signal: modeOpt[0] = 0 for the default group of carriers, n > 0 for a single bearer (index n - 1); modeVal[0] = C/N0 in dBHz (0 = 70).
#pragma once
#include "dect2/bgan/bgan_phy.h"
#include "dect2/bgan/bgan_sig.h"
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2::bgan {

struct GenConfig {
    double fs = 1e6;
    int bearer = 0, subtype = 8;   // subtype of the unique word (8 = R)
    double offsetHz = 0;
    double driftHzPerSec = 0;
    double amplitude = 1.0;
    double clockPpm = 0;           // symbol clock error of the transmitter
    double fadeHz = 0;             // Rician-like amplitude and phase fade rate (0 = none)
    double fadeDepth = 0;          // 0..1
    uint32_t seed = 1;
    TxSig sig;
    bool signalling = true;
};

class Generator {
public:
    explicit Generator(const GenConfig& c);
    void generate(cf32* out, size_t n);          // adds into out
private:
    GenConfig c_;
    const Bearer& b_;
    FrameLayout lay_;
    std::vector<float> rrc_;
    std::vector<cf32> sym_;                      // symbols of the current frame and history of 16
    int frame_ = 0, fn_ = 0;
    double t_ = 0;                               // in symbols, into the symbol stream
    double ph_ = 0, phFade_ = 0, drift_ = 0, sinceStart_ = 0;
    std::vector<cf32> stream_;
    void nextFrame();
};

std::unique_ptr<ModeSynth> makeSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2::bgan
namespace dect2 { std::unique_ptr<ModeSynth> makeBganSynth(const SynthConfig& cfg, double sampleRate); }
