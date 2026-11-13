// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CubeSat telemetry test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// makeCubesatSynth(): made-up satellites send one frame of each framing on one FM channel, in an endless cycle: AX.25 at 1200 baud AFSK
// and 9600 baud G3RUH, AX100 Mode 5 (GMSK, 9600 and 4800 baud), AX100 Mode 6 (G3RUH 9600) and a CCSDS Reed-Solomon frame (GMSK 9600).
// The carrier sits at the mode's tune offset below the radio's centre (plus cfoHz); the cycle is cubesatGenCycle() with its length
// cubesatGenCycleSec().
//
// SynthConfig: snrDb = carrier to noise in 25 kHz; cfoHz = carrier offset (Doppler); modeOpt[0] = 0 every framing, 1 + cubesat::Framing
// only that one.
#pragma once
#include "cubesat_proto.h"
#include "dect2/engine/mode_synth.h"
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct CubeGenFrame {
    int framing = 0;
    int baud = 1200;
    std::string from;                   // AX.25 source callsign ("" for the others)
    std::vector<uint8_t> data;          // what the receiver reports as CubeFrame::data
};

std::vector<CubeGenFrame> cubesatGenCycle(int mode);
double cubesatGenCycleSec(int mode);
std::unique_ptr<ModeSynth> makeCubesatSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
