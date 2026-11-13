// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Inmarsat all-carriers test signal: the synthetic source plays what the mode is for, several carriers in ONE wide capture:
//   the Aero test signal (P channels at 600, 1200 and 10500 bit/s, aero_gen.h) at the centre, a second Aero group of 10500 bit/s about 23 percent of the
//   sample rate below it, an Inmarsat-C NCS channel with one extra channel 15 percent above, and a wide unmodulated-looking carrier 30 percent above
//   that decodes as nothing (a stand-in for a C channel), or, with modeOpt[2] = 1, a real Aero C channel there (aero_cgen.h: 8400 bit/s voice frames).
// SynthConfig: modeVal[0] = Eb/N0 of the data carriers in dB (0 = 17). Nothing is transmitted.
#pragma once
#include "dect2/engine/mode_synth.h"
#include <memory>

namespace dect2 {
std::unique_ptr<ModeSynth> makeInmsatSynth(const SynthConfig& cfg, double sampleRate);
}
