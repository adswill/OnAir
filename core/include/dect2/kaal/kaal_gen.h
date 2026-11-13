// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL test signal for the engine's synthetic source: the real transmitter (VoiceTx: voice codec, 16-QAM LDPC, sweep with the station ID, beacon slice) sending
// a speech-like test tone sequence, at the radio rate, with a carrier offset (the radios' oscillators) and a little noise.
//
// SynthConfig (makeKaalSynth):
//   snrDb       noise: the signal's rms over the noise rms across the whole sampled band (30 = a little noise)
//   cfoHz       added to the built-in carrier offset of +1200 Hz
//   modeOpt[0]  voice bitrate in kbit/s: 12, 16, 24 or 32 (0 = 16); 24 and 32 are 16 ksym/s, about 20 kHz wide
#pragma once
#include "dect2/engine/mode_synth.h"
#include <memory>

namespace dect2 {

std::unique_ptr<ModeSynth> makeKaalSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
