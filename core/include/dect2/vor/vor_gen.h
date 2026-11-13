// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VOR / ILS test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// The AM envelope of the beacon (ICAO Annex 10 Volume I) on a carrier at the dial frequency (which sits at -tuneOffsetHz in the samples, as
// the radio is tuned above it), over complex white noise:
//  - conventional VOR: 1 + 0.3 cos(W t - radial) + 0.3 cos(w9960 t + 16 sin(W t)) (the variable AM lags the FM reference by the radial)
//  - Doppler VOR: the carrier's 30 Hz AM is the reference, 0.3 cos(W t), and the subcarrier is the sum of an upper and a lower sideband
//    whose phases follow the Doppler shift of antennas turning anticlockwise on a ring: 16 sin(W t + radial). Same bearing, roles swapped.
//  - localizer: 1 + (0.2 + DDM/2) cos(2 pi 90 t) + (0.2 - DDM/2) cos(2 pi 150 t); glide slope the same with 0.4; marker 95 % keyed tone
//  - the 1020 Hz identification in Morse (10 % depth) and, for a VOR, a voice-like ATIS (20 %)
//  - optionally a reflection: the beacon's signal as seen from another azimuth, weaker, its carrier phase turning (scalloping)
//
// SynthConfig (makeVorSynth):
//   snrDb       carrier to noise in 25 kHz
//   cfoHz       carrier offset
//   modeOpt[0]  0 conventional VOR, 1 Doppler VOR, 2 localizer, 3 glide slope, 4 outer, 5 middle, 6 inner marker
//   modeVal[0]  the radial (VOR) in degrees, or the DDM (ILS)
#pragma once
#include "dect2/engine/mode_synth.h"
#include "vor_tel.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace dect2 {

struct VorGenConfig {
    double rate = 250000;
    double dialOffsetHz = 0;     // where the carrier sits in the samples (before cfoHz)
    double snrDb = 40;           // carrier to noise in 25 kHz; >= 99: no noise
    double cfoHz = 0;
    double amp = 0.2;            // carrier amplitude
    uint32_t seed = 1;
    int kind = 0;                // 0 conventional VOR, 1 Doppler VOR, 2 localizer, 3 glide slope, 4 outer, 5 middle, 6 inner marker
    double radialDeg = 0;        // VOR
    double ddm = 0;              // ILS: m90 - m150
    double tone30Hz = 30;        // the 30 Hz (Annex 10: +-1 %)
    double sub9960Hz = 9960;     // the subcarrier (+-1 %)
    std::string ident = "DXB";   // Morse, 1020 Hz; empty: none
    double identWpm = 7;
    double identPeriod = 10;     // seconds from the start of one identification to the next
    double identHz = 1020;
    bool voice = true;           // VOR: voice-like audio on the carrier
    double reflDb = -99;         // a reflection, relative to the direct signal (-99: none)
    double reflRadialDeg = 0;    // the azimuth it comes from (its own radial)
    double reflFadeHz = 0;       // how fast its carrier phase turns against the direct one
};

class VorGenerator {
public:
    explicit VorGenerator(const VorGenConfig& c);
    ~VorGenerator();
    void generate(cf32* out, size_t n);
    double timeSec() const;
    static double noiseSigma(const VorGenConfig& c);   // per component
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeVorSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
