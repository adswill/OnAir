// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 5G NR cells test signal: NR SS/PBCH blocks (PSS, SSS, PBCH with its DMRS and the MIB) built from the same specification tables the
// receiver reads, with random QPSK around them standing in for the rest of the carrier. Nothing is transmitted: the samples only exist in
// memory.
//
// SynthConfig (makeNrSynth):
//   snrDb       signal to noise inside the SSB
//   cfoHz       the radio's tuning error
//   modeOpt[0]  scene: 0 two n78 cells (30 kHz SSBs, 8 beams) at the centre and 5.76 MHz above it (GSCN 7875 and 7879 at the default
//               3541.44 MHz), 1 one n28 cell (15 kHz, 4 beams) at the centre, 2 one barred n78 cell without SIB1 (k_SSB 31)
#pragma once
#include "dect2/engine/mode_synth.h"
#include "nr_phy.h"
#include <memory>

namespace dect2 {

struct SynthConfig;

struct NrTestCell { nr::GenCell g; double offsetHz; };
std::vector<NrTestCell> nrTestScene(int scene);

std::unique_ptr<ModeSynth> makeNrSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
