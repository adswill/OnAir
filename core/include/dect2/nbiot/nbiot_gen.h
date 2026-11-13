// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NB-IoT test signal: one NB-IoT carrier (NPSS, NSSS, NRS, NPBCH with the MIB-NB, and the NPDSCH carrying SIB1-NB, random QPSK in the other
// downlink subframes), built from the same specification tables the receiver reads. It uses the test network code 001-01, not a real
// operator. Nothing is transmitted: the samples only exist in memory.
//
// SynthConfig (makeNbiotSynth):
//   snrDb       signal to noise per resource element (the 180 kHz carrier)
//   cfoHz       the radio's tuning error (on top of the raster offset of the scene)
//   sroPpm      sample clock error
//   modeOpt[0]  scene: 0 standalone, 1 guard band with 2 NRS ports, 2 in-band (same PCI), 3 in-band (other PCI, 4 LTE CRS ports)
#pragma once
#include "dect2/engine/mode_synth.h"
#include "nbiot_phy.h"
#include <memory>

namespace dect2 {

struct NbTestTruth { int pci, ports, mode, mcc, mnc, band, sched; uint32_t tac, cellId; double rasterKhz; };
// The test cell of a scene (its generator configuration) and what the receiver should read from it
nbiot::GenCfg nbiotTestCell(int scene, NbTestTruth* truth = nullptr, int pciOverride = -1);

std::unique_ptr<ModeSynth> makeNbiotSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
