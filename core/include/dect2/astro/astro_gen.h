// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Test signal of the radio astronomy tool (makeAstroSynth), for a radio tuned to the hydrogen line (1420.405751 MHz): Gaussian noise
// whose spectrum has a sloped, slightly curved baseline (the receiver's band shape), plus a weak Gaussian line kAstroLineDb under the
// noise at the frequency of the radial velocity kAstroLineKms (20 km/s wide). SynthConfig::modeOpt[0] = 1: off source (no line).
#pragma once
#include "dect2/engine/mode_synth.h"
#include <memory>

namespace dect2 {

constexpr double kAstroLineKms = -42.0;       // radio convention: negative = approaching, the line above the rest frequency
constexpr double kAstroLineDb = -20.0;        // line peak relative to the noise in the same bin (1 %: a minute of integration shows it)
constexpr double kAstroLineFwhmKms = 20.0;
constexpr double kAstroSlope = 0.30, kAstroCurve = -0.10;   // baseline 1 + slope x + curve x^2, x = -1 .. 1 across the band
std::unique_ptr<ModeSynth> makeAstroSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
