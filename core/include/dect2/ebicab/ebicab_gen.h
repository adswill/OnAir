// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// EBICab / Eurobalise test signal. Nothing is transmitted: the samples only exist in memory or in a file.
// A passing train as seen by the up-link: bursts in which the balise repeats its telegram (3.951 / 4.516 MHz FSK, 564.48 kbit/s, starting at a
// random place of the telegram, with a clock error and jitter), separated by quiet gaps, in white noise, with a carrier offset and drift.
// SynthConfig (makeEbicabSynth): snrDb is Eb/N0 in dB, cfoHz the carrier offset, modeOpt[0] 0 long telegrams, 1 short, 2 alternating long / short,
// 3 bursts of random bits (no telegram, for the negative tests).
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct EbicabGenConfig {
    double rate = 10e6;
    double dialOffsetHz = 0;         // where 4.234 MHz sits in the samples
    double cfoHz = 0;
    double driftHzPerSec = 0;
    double clockPpm = 0;             // data rate error (the standard allows 2.5 %)
    double ebn0Db = 99;              // >= 99: no noise
    double amp = 0.4;
    bool invert = false;             // the two frequencies swapped
    double burstSec = 0.006;         // length of a burst (about 3 long telegrams)
    double gapSec = 0.020;           // quiet time between bursts
    double jitterNs = 0;             // random timing jitter per bit (rms)
    std::vector<std::vector<uint8_t>> telegrams;   // each burst repeats the next one (ebicab::encode), then the list starts again
    bool randomBits = false;         // bursts of random bits instead
    uint32_t seed = 1;
};

class EbicabGenerator {
public:
    explicit EbicabGenerator(const EbicabGenConfig& c);
    ~EbicabGenerator();
    void generate(cf32* out, size_t n);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// A balise telegram with the header fields of a made-up balise, encoded (long or short)
std::vector<uint8_t> ebicabTestTelegram(bool longFmt, int nidBg, int nPig);
std::unique_ptr<ModeSynth> makeEbicabSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
