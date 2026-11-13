// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAFT test signal for the engine's synthetic source: a small demo text file (200 kHz width) as one session, sent again and again (OPEN, START, data bursts, FINISH, with OPEN and START before every 8th burst), 1 s gap between the passes. The session number stays the same, so a receiver that has the file ignores the repeat.
// It is the real transmitter's output (kaft_txgen.h, the one the TX panel uses) at the radio rate, with a carrier offset (the radios' oscillators) and a
// little noise, played as a seamless loop (synth_loop.h). Nothing is transmitted: the samples only exist in memory or in a file.
//
// SynthConfig (makeKaftSynth):
//   snrDb       noise: the signal's rms over the noise rms across the whole sampled band (30 = a little noise)
//   cfoHz       added to the built-in carrier offset of +1500 Hz
//   modeOpt[0]  profile: 0 = the default (P2, QPSK 1/2), n = profile n-1 (1 .. 10 = P0 .. P9)
#pragma once
#include "dect2/engine/mode_synth.h"
#include <memory>

namespace dect2 {

std::unique_ptr<ModeSynth> makeKaftSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
