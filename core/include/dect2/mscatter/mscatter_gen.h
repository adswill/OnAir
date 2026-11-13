// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meteor scatter test signal: a CW beacon (offset a little from the radio's centre, like a real radio's frequency error) in noise, and meteor echoes
// that appear as short Doppler-shifted copies of it: sharp rise, exponential decay (underdense) and some long flat ones (overdense), now and then
// a broadband interference burst that must not be counted. Nothing is transmitted: the samples only exist in memory or a file.
//
// SynthConfig (makeMscatterSynth): snrDb (beacon over the noise in 100 Hz), cfoHz (beacon offset from the centre; 0: +420 Hz);
// modeOpt[0]: 0 a ping every few seconds (the screen test), 1 about one a minute, 2 none (beacon only);
// modeOpt[1]: 1 adds a broadband burst every ~20 s.
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct MsTruePing { double startSec, durSec, dopplerHz, peakSnrDb; bool overdense; };

struct MscatterGenConfig {
    double rate = 1e6;
    double snrDb = 40;                  // beacon over the noise in 100 Hz; >= 99: no noise
    double carrierHz = 420;             // beacon offset from the centre
    double amp = 0.3;                   // beacon amplitude
    double meanGapSec = 3.0;            // between pings (exponential); <= 0: none
    double firstSec = 4.0;              // no ping before (the detector learns its background)
    bool carrierOn = true;              // false: no beacon (tests)
    bool interference = false;
    uint32_t seed = 1;
    bool pingsOnlyAt = false;           // true: use `fixed` instead of random pings
    std::vector<MsTruePing> fixed;
};

class MscatterGenerator {
public:
    explicit MscatterGenerator(const MscatterGenConfig& c);
    ~MscatterGenerator();
    void generate(cf32* out, size_t n);
    const std::vector<MsTruePing>& scheduled() const;   // every ping placed so far
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeMscatterSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
