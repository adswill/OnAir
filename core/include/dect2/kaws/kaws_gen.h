// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAWS test signal for the engine's synthetic source: a chat message from ONAIR (a short beacon every other pass) in the UTC slots of the profile. The slot grid is the computer clock's, the same one the receiver follows: the generator waits for the next even slot of that clock (up to two slot lengths), sends the burst 1.5 s after the slot start, and builds a new one when the message has been sent.
// It is the real transmitter's output (kaws_txgen.h, the one the TX panel uses) at the radio rate, with a carrier offset (the radios' oscillators) and a
// little noise, played as a seamless loop (synth_loop.h). Nothing is transmitted: the samples only exist in memory or in a file.
//
// SynthConfig (makeKawsSynth):
//   snrDb       noise: the signal's rms over the noise rms across the whole sampled band (30 = a little noise)
//   cfoHz       added to the built-in carrier offset of +30 Hz (the dial sits 20 kHz below the radio's centre: the mode's tuning offset)
//   modeOpt[0]  profile: 0 = the default (P3 Chat, 30 s slots), n = profile n-1 (1 .. 6 = P0 .. P5)
#pragma once
#include "dect2/engine/mode_synth.h"
#include <memory>

namespace dect2 {

std::unique_ptr<ModeSynth> makeKawsSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
