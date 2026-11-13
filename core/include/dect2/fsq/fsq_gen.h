// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FSQ test signal (see kbd33_gen.h for the generator and its channel options). The engine's simulator sends a FSQ message at speed 2.
//
// SynthConfig (makeFsqSynth):
//   snrDb       added to the level of the signal (given in the band of the 33 tones)
//   cfoHz       the radio's tuning error
//   modeOpt[0]  1 + the speed index to send (0 = the default of the test signal)
#pragma once
#include "dect2/fsq/kbd33_gen.h"
#include "dect2/engine/mode_synth.h"
#include <memory>
#include <vector>

namespace dect2 {

std::vector<Kbd33GenSignal> fsqTestSignals();
std::unique_ptr<ModeSynth> makeFsqSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
