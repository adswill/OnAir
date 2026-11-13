// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Test signal of the spectrum explorer (makeSpecxSynth): a QPSK carrier (root-raised-cosine, roll-off 0.35, fs / 16 symbols a second)
// at -0.2 fs, an unmodulated carrier at +0.15 fs, and noise. SynthConfig::snrDb is the QPSK carrier's SNR in its symbol-rate bandwidth.
#pragma once
#include "dect2/engine/mode_synth.h"
#include <memory>

namespace dect2 {

constexpr double kSpecxQpskOffset = -0.2, kSpecxCwOffset = 0.15, kSpecxSymDiv = 16;   // fractions of the sample rate
std::unique_ptr<ModeSynth> makeSpecxSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
