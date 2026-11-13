// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Globalstar test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// makeGlobalstarSynth(): band-limited noise of 1.23 MHz (the look of a CDMA terminal uplink) on the channels of the band that the sample rate covers.
// SynthConfig: snrDb = signal to noise in the channel bandwidth; cfoHz = extra Doppler on every channel; modeVal[0] = time scale (0 = 1: 10 plays a
// pass in a tenth of the time); modeOpt[0] = scenario: 0 a pass (one channel is active throughout and its Doppler sweeps from +30 kHz to
// -30 kHz over 40 s, a second channel carries bursts of 300 ms every 800 ms), 1 steady (one channel, constant Doppler = cfoHz),
// 2 silence (noise only). modeOpt[1] = the channel used (1 .. 13, 0 = the one nearest the centre of the band sampled).
#pragma once
#include "dect2/engine/mode_synth.h"
#include <memory>

namespace dect2 {

constexpr double kGsPassSec = 40.0;
constexpr double kGsPassDopplerHz = 30000.0;
double globalstarGenDoppler(double sec);   // the pass's Doppler at this time (before cfoHz is added)
std::unique_ptr<ModeSynth> makeGlobalstarSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
