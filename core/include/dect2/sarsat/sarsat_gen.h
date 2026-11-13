// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// COSPAS-SARSAT 406 MHz test signal. Nothing is transmitted: the samples only exist in memory or in a file. The messages are made-up beacons
// (a test protocol identification, never a registered one).
//
// makeSarsatSynth(): an endless loop of bursts: 160 ms unmodulated carrier, then the message at 400 bit/s Manchester, phase modulated +-1.1 rad.
// The carrier sits sarsatTuning().tuneOffsetHz below the radio's centre plus SynthConfig::cfoHz. snrDb is the carrier power over the noise power in 2.4 kHz.
//
// SynthConfig::modeOpt:  [2] 1 = second-generation beacons (SGB, 1 s DSSS bursts, test beacons from sgbTestBeacons) instead of the first generation
// SynthConfig::modeOpt:  [0] which test beacon to repeat (1..N, 0 = all in turn)   [1] 1 = phase polarity reversed   [3] noise seed, 0 = 1
// SynthConfig::modeVal:  [0] bit rate error in percent (0 = exact)   [1] seconds between bursts (0 = 3 s; real beacons repeat about every 50 s)
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/sarsat/sarsat_proto.h"
#include "dect2/sarsat/sgb_proto.h"
#include <complex>
#include <memory>
#include <vector>

namespace dect2::sarsat {
// The messages of the test signal, in the order it plays them
std::vector<Spec> testBeacons();
// The phase waveform of one burst (carrier, then the message) as samples at `rate`: complex unit-amplitude values, used by the tests and the synth
std::vector<std::complex<float>> burstSamples(const std::vector<uint8_t>& messageBits, double rate, double carrierHz, double rateErrorPct = 0, bool inverted = false);

// Second-generation beacons (C/S T.018): made-up test beacons, and one burst (1 s, DSSS O-QPSK with rectangular chips) as samples at `rate`, unit power.
// chipPpm: chip rate (and nothing else) off by this many parts per million, selfTest: the self-test PRN segments.
std::vector<SgbSpec> sgbTestBeacons();
std::vector<std::complex<float>> sgbBurstSamples(const std::vector<uint8_t>& bits250, double rate, double carrierHz, bool selfTest = false, double chipPpm = 0);
constexpr double kSgbOffsetHz = 22000.0;   // 406.050 MHz relative to the 406.028 MHz dial
}

namespace dect2 {
std::unique_ptr<ModeSynth> makeSarsatSynth(const SynthConfig& cfg, double sampleRate);
}
