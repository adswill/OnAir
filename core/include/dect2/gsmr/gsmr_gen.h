// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GSM-R test signal: base station carriers with their broadcast channels (FCCH, SCH, BCCH with System Information 1 to 4 on a real 51 frame
// schedule, the other slots filled with dummy bursts), built from the same specification tables the receiver reads. It uses the test
// network code 001-01, not a real operator. Nothing is transmitted: the samples only exist in memory.
//
// SynthConfig (makeGsmrSynth):
//   snrDb       signal to noise inside one 200 kHz channel of the strongest carrier
//   cfoHz       the radio's tuning error
//   modeOpt[0]  scene: 0 two railway cells (ARFCN 960, 968), 1 one cell at the lower band edge (955), 2 three cells, the third 12 dB weaker (ARFCN 957, 964, 971)
// The carriers sit at their ARFCN frequencies relative to a radio centred on 922.8 MHz (the mode's default).
#pragma once
#include "gsmr_si.h"
#include "dect2/engine/mode_synth.h"
#include <memory>
#include <vector>

namespace dect2 {

struct SynthConfig;

struct GsmrCellCfg {
    int arfcn = 960, ncc = 1, bcc = 2;
    gsmr::Si3 si;                       // cell identity, location area, access parameters
    std::vector<int> cellAlloc, neigh;
    int nccPermitted = 0xFF;
    uint32_t fn0 = 0;                   // the frame number at the start of the signal
    int phase = 0;                      // samples (at 8 per symbol) into the first frame
    float amp = 0.25f;
    bool ccchLoad = true;               // some paging and assignment messages (without identities) on the CCCH
};

constexpr double kGsmrTestCentreHz = 922.8e6;

int gsmrTestCells(int scene);
GsmrCellCfg gsmrTestCell(int scene, int index);

namespace gsmr {
constexpr double kNativeRate = 8.0 * 1625000.0 / 6.0;   // 8 samples per symbol
// n frames of one base station at 8 samples per symbol (10000 per frame) starting at frame number fn
std::vector<cf32> btsFrames(const GsmrCellCfg& c, uint32_t fn, int n);
}

std::unique_ptr<ModeSynth> makeGsmrSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
