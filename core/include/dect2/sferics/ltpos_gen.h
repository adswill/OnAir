// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Lightning position (network) simulation: strikes at known positions and times, rendered as the IQ that each station's radio would deliver:
// the propagation delay to that station (fractional samples), its sample clock running fast or slow (ppm), its start time, its noise, and
// optionally a common-view MSK reference station at an offset in the same band. Also the test signal of the mode (one station's view).
// Nothing is transmitted; the samples exist only in memory.
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/sferics/ltpos.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2::ltpos {

struct SimStrike {
    double lat = 0, lon = 0;
    double tS = 0;               // true time of the discharge, seconds from the scene's origin
    double ampAt100km = 0.5;     // peak amplitude at 100 km; it falls as 1 / distance
};

struct SimStation {
    double lat = 0, lon = 0;
    double startS = 0;           // true time of sample 0
    double ppm = 0;              // the radio's sample clock error (+ = fast)
    double cfoHz = 0;            // extra carrier offset of the radio
    double noiseRms = 0.01;
    double refAmp = 0;           // amplitude of the reference station here (0: none)
    double refOffsetHz = 0;      // the reference carrier relative to the tuned centre
    uint32_t seed = 1;
};

struct MskState { int64_t k = 0; double theta = 0; bool init = false; };

struct SimScene {
    std::vector<SimStrike> strikes;
    PropModel pm;
    double tauUs = 60;           // decay of a sferic
    double riseUs = 12;          // raised-cosine rise (a band-limited leading edge)
    double oscHz = 25e3;         // oscillation in the band, relative to the tuned centre
    double maxAmp = 0.9;
    RefTx ref;                   // the reference transmitter (baud 0: none)
    double refEpochS = 0.0011;   // its symbol boundaries: refEpochS + k T (true time)
    uint32_t refSeed = 7;
};

double simArrivalS(const SimScene& sc, const SimStrike& k, const SimStation& st);   // true arrival time at the station
double simSampleTime(const SimStation& st, double trueS);                           // the station's sample time (n / rate) of a true time
double simTrueTime(const SimStation& st, double sampleS);
// n samples of the station's radio, from sample index first (so a long scene can be rendered in pieces; pass the same MskState along)
void simRender(const SimScene& sc, const SimStation& st, double rate, uint64_t first, size_t n, std::complex<float>* out, MskState* msk);

// the mode's test signal: one station's view: sferics (modeVal[0] strikes per second, default 2) and, with modeOpt[0] = 1, an MSK reference
// at modeVal[1] Hz from the centre (default 50 kHz) at 200 Bd
std::unique_ptr<ModeSynth> makeLtposSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2::ltpos
