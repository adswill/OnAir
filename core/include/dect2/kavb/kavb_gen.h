// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAVB test signal for the engine's synthetic source: the synthetic audio-video test stream (about 3 s of media time) from the real KAVB transmitter, 1 s gap between the passes.
// It is the real transmitter's output (kavb_txgen.h, the one the TX panel uses) at the radio rate, with a carrier offset (the radios' oscillators) and a
// little noise, played as a seamless loop (synth_loop.h). Nothing is transmitted: the samples only exist in memory or in a file.
//
// SynthConfig (makeKavbSynth):
//   snrDb       noise: the signal's rms over the noise rms across the whole sampled band (30 = a little noise)
//   cfoHz       added to the built-in carrier offset of +3000 Hz
//   modeOpt[0]  profile: 0 = A (the default), n = profile n-1 (1 .. 4 = A .. D)
#pragma once
#include "dect2/engine/mode_synth.h"
#include <memory>

namespace dect2 {

std::unique_ptr<ModeSynth> makeKavbSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
