// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAIT test signal for the engine's synthetic source: the built-in test picture as a 10 s burst, then 16 s of silence (a decode on a slow machine takes a few seconds), again and again (the transmitter's own cycle).
// It is the real transmitter's output (kait_txgen.h, the one the TX panel uses) at the radio rate, with a carrier offset (the radios' oscillators) and a
// little noise, played as a seamless loop (synth_loop.h). Nothing is transmitted: the samples only exist in memory or in a file.
//
// SynthConfig (makeKaitSynth):
//   snrDb       noise: the signal's rms over the noise rms across the whole sampled band (30 = a little noise)
//   cfoHz       added to the built-in carrier offset of +500 Hz
//   modeOpt[0]  picture profile: 0 balanced, 1 robust, 2 quality, 3 direct
#pragma once
#include "dect2/engine/mode_synth.h"
#include <memory>

namespace dect2 {

std::unique_ptr<ModeSynth> makeKaitSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
