// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Test signal of the QO-100 tools (makeQo100Synth): what a Ku LNB with a 9750 MHz oscillator hands the radio. The LNB's oscillator is
// off by kQo100SynthErrHz and drifts slowly (actual LO = 9750 MHz + error), so everything arrives that much low. On the narrowband
// transponder: the CW beacons at 10489.500 and 10490.000 MHz, the 400 bit/s BPSK beacon at 10489.750, a steady test carrier at
// kQo100TestCarrierHz and four SSB stations (upper sideband, three voice-like tones each with a syllable envelope); on the wideband
// transponder: the DATV beacon (1.5 Msym/s at 10491.5 MHz) and a 333 ksym/s carrier at 10494.75 MHz as flat-topped noise; white noise.
// The radio's centre: 10494.0 MHz at 9 Msps and up (the whole downlink), else 10490.1 MHz (the narrowband transponder).
// SynthConfig: modeOpt[0] = 1 noise only; modeVal[0] Hz added to the LNB error, modeVal[1] drift Hz/min (0: kQo100SynthDriftHzPerMin),
// modeVal[2] the radio's centre in RF MHz (0: as above), modeVal[3] the nominal LO in MHz (0: 9750; above the downlink: inverted).
#pragma once
#include "dect2/engine/mode_synth.h"
#include <memory>

namespace dect2 {

constexpr double kQo100SynthErrHz = 27000.0;           // actual LO - nominal at the start
constexpr double kQo100SynthDriftHzPerMin = 120.0;     // a warming LNB: 2 Hz per second
constexpr double kQo100TestCarrierHz = 10489.650e6;    // true RF
constexpr double kQo100SsbDialsHz[4] = {10489.540e6, 10489.600e6, 10489.820e6, 10489.905e6};
constexpr double kQo100Datv2Hz = 10494.750e6, kQo100Datv2Sym = 333e3;

double qo100SynthErrHz(const SynthConfig& cfg, double tSec);   // the LNB error the test signal has at that time
double qo100SynthCentreHz(const SynthConfig& cfg, double sampleRate);   // the radio centre it assumes (RF)
std::unique_ptr<ModeSynth> makeQo100Synth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
