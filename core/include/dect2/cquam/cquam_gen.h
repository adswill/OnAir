// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AM stereo test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// One medium-wave station with C-QUAM stereo: left and right tones (440 and 660 Hz, 0.35 each) on the carrier as z = (1 + L + R) +
// j (L - R + pilot) with the 25 Hz pilot at 5 %, over white noise. The radio sits tuneOffsetHz above the dial.
//
// SynthConfig (makeCquamSynth):
//   snrDb       carrier over the noise in 10 kHz (the default channel filter, 5 kHz either side)
//   cfoHz       the radio's tuning error
//   modeOpt[0]  0 C-QUAM stereo, 1 plain AM (mono, no pilot), 2 noise only
//   modeOpt[1]  1 the left channel silent (a separation check)
//   modeVal[0]  Hz the station sits from the dial (0)
//   modeVal[1]  fading depth 0 .. 1: the whole signal swings between 1 and 1 - depth (0 none)
//   modeVal[2]  fading rate in Hz (0.5)
//   modeVal[3]  Hz of the left tone (440; the right tone is 1.5 times that)
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstddef>
#include <memory>

namespace dect2 {

constexpr double kCquamSynthAmp = 0.3;     // carrier amplitude (full scale 1)
constexpr double kCquamPilotDepth = 0.05;  // the pilot in the quadrature part, relative to the carrier

std::unique_ptr<ModeSynth> makeCquamSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
