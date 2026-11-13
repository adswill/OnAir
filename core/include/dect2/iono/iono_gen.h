// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Test signal of the ionospheric sounder mode (makeIonoSynth): two chirp sounders of different rates and timing, each heard on two
// ionospheric paths (delayed echoes, the delays constant over frequency), in Gaussian noise. Time starts at 0 with the first sample.
// The generator places the sweeps for a radio tuned to SynthConfig::modeVal[0] MHz (0: kIonoSynthDefMhz).
// SynthConfig::modeOpt[0] = 1: noise only.
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

constexpr double kIonoSynthDefMhz = 10.0;
struct IonoSynthSounder { double rateHz, periodSec, offsetSec; double delaySec[2]; double amp[2]; };
// A: 100 kHz/s every 30 s at +2 s; B: 500 kHz/s every 20 s at +7 s. Each sweep starts at 0 MHz and lasts 95 % of the period
// (a quick scene: real sounders repeat every few minutes and sweep the whole 0 .. 30 MHz).
constexpr IonoSynthSounder kIonoSynth[2] = {
    {100e3, 30, 2, {1.6e-3, 2.4e-3}, {1.0, 0.6}},
    {500e3, 20, 7, {1.8e-3, 2.9e-3}, {1.0, 0.5}},
};
constexpr double kIonoSynthAmp = 0.063;   // of the strongest path: 10 dB under the noise (rms 0.2) per sample
std::unique_ptr<ModeSynth> makeIonoSynth(const SynthConfig& cfg, double sampleRate);

// Synthetic ionograms of a layered ionosphere (tests of the scaling, iono_scale.h): parabolic E, F1 and F2 layers (the plasma frequency
// profile is the largest of the three at each height), virtual heights by integrating the group index 1 / sqrt(1 - fN^2 / f^2) up to the
// reflection (no magnetic field; the X trace as the O trace at the frequency sqrt(f (f - fH))). Vertical (distKm 0): echoes at group
// range 2 h', the ground wave at 0 and the second reflection at 4 h'. Oblique: Martyn's theorem over a spherical earth: an n-hop ray of
// equivalent vertical frequency fv reaches h'(fv) and is heard at fv / cos(i) with the group path of the mirror there; F rays that the
// E layer turns back are left out. Cells: noise averaged over `blocks` exponential powers, echoes snrDb over it, in dB over each
// column's median like Processor::fill; NAN outside fLoMhz .. fHiMhz.
struct IonoLayerModel {
    double foE = 3.0, hmE = 110, ymE = 20;           // MHz, km (0 foE: no E layer)
    double foF1 = 0, hmF1 = 200, ymF1 = 50;
    double foF2 = 7.0, hmF2 = 320, ymF2 = 100;
    double fH = 1.3;                                  // gyrofrequency for the X trace (0: no X)
    double distKm = 0;                                // 0 vertical, else the ground distance of an oblique path
    int hops = 2;                                     // vertical: reflections drawn; oblique: modes up to this many hops
    double snrDb = 15, xDb = -5, hopDb = -6;          // echo over the noise; the X trace and each further hop relative to it
    int blocks = 3;                                   // noise blocks averaged per cell
    double fLoMhz = 1.0, fHiMhz = 20.0;              // swept part
    bool noiseOnly = false;
    std::vector<double> carriersMhz;                  // interfering carriers: columns lit at every delay
    uint64_t seed = 1;
};
double ionoVirtualHeightKm(const IonoLayerModel& m, double fMhz, bool x = false);   // NAN: penetrates (or below the E layer foot)
double ionoPathMufMhz(const IonoLayerModel& m, int layer /* 1 E, 3 F2 */, int hops); // the nose of an oblique mode
std::vector<float> makeIonoLayerIonogram(const IonoLayerModel& m);                   // iono::kRows x iono::kCols dB

} // namespace dect2
