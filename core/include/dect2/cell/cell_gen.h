// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cell towers test signal: one LTE eNodeB carrier with its broadcast channels (PSS / SSS, reference signals, PBCH, PCFICH, PDCCH and
// PDSCH on SI-RNTI with SIB1 to SIB5), built from the same specification tables the receiver reads. It uses the test network code
// 001-01, not a real operator. Nothing is transmitted: the samples only exist in memory.
//
// SynthConfig (makeCellSynth):
//   snrDb       signal to noise inside the carrier
//   cfoHz       the radio's tuning error
//   modeOpt[0]  scene: 0 a 10 MHz cell with 2 ports, 1 a 5 MHz cell with 4 ports, 2 a 1.4 MHz cell with 1 port, 3 a 20 MHz cell
#pragma once
#include "cell_lte.h"
#include "dect2/engine/mode_synth.h"
#include <memory>

namespace dect2 {

struct SynthConfig;

struct CellTestTruth { int pci, nRb, ports, mcc, mnc, band; uint32_t tac, cellId; std::vector<int> intraPci; std::vector<int> interEarfcn; };
// The test cell of a scene: its generator configuration (SIB1, SI-1 with SIB2, SI-2 with SIB3, SI-3 with SIB4 and SIB5 on DCI 1C) and what
// the receiver should read from it. pciOverride >= 0 gives another cell id (two cells at once in the tests).
cell::LteGenCfg cellTestCell(int scene, CellTestTruth* truth = nullptr, int pciOverride = -1);

std::unique_ptr<ModeSynth> makeCellSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
