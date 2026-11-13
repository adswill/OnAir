// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Test signal of the general receiver (makeGeneralSynth): one station on the dial frequency (the radio sits tuneOffsetHz above it),
// modulated by a steady tone, over white noise (SynthConfig::snrDb measured in 12.5 kHz).
// SynthConfig: modeOpt[0] the modulation (GeneralDemod: 0 NFM, 1 WFM, 2 AM, 3 USB, 4 LSB, 5 CW), modeOpt[1] = 1 noise only;
// modeVal[0] Hz the station sits from the dial, modeVal[1] tone Hz (0: 1000; CW: unused, a steady carrier), modeVal[2] modulation
// as a fraction (0: 0.5): FM deviation of the receiver's reference (NFM 5 kHz, WFM 75 kHz), AM depth. USB / LSB: the tone is a
// single sideband carrier at dial +- tone.
#pragma once
#include "dect2/engine/mode_synth.h"
#include <memory>

namespace dect2 {

constexpr double kGeneralSynthAmp = 0.3;   // carrier amplitude (full scale 1)

std::unique_ptr<ModeSynth> makeGeneralSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
