// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "What satellite is this?": given a carrier followed through a pass (DopplerSample list), rank every satellite with known elements by how well its
// predicted Doppler curve fits. The nominal transmit frequency is unknown, so a constant offset is solved for each candidate; what remains is the shape
// of the curve, which only a satellite at the right place in the right orbit can produce. The best candidates are then refined with fitDopplerOrbit.
// The same ranking takes a bistatic geometry (transmitter elsewhere, GRAVES radar echoes).
// Limits: Sgp4 is near-earth only, so geostationary and other deep-space objects cannot be candidates; satellites flying together (a launch train) have
// the same curve and are reported as ambiguous rather than picked between.
#pragma once
#include "dect2/geo/doppler_fit.h"
#include "dect2/geo/passes.h"
#include <string>
#include <vector>

namespace dect2 {
namespace satmisc {

struct SatIdOptions {
    double refHz = 0;             // the radio frequency the samples are relative to (only sets the Doppler scale)
    bool fitOffset = true;        // solve a constant offset (unknown transmit frequency); false: the samples are already relative to the true carrier
    double maxOffsetHz = 25000;   // a candidate needing more offset than this is dropped
    double goodRmsHz = 60;        // a match is believable under this residual
    double minElDeg = -2;         // the candidate must be above this at every sample
    bool bistatic = false;        // Doppler = -f (range rate to tx + range rate to rx) / c
    Observer tx;                  // the transmitter (bistatic only)
    int maxResults = 8;
    int refine = 3;               // how many of the best to refine (0: none; never done for bistatic geometry)
};

struct SatIdMatch {
    Tle tle;
    double rmsHz = 0;             // residual of the curve after the offset
    double offsetHz = 0;          // the solved constant: transmit frequency minus refHz, plus any receiver error
    double peakElDeg = 0;
    double refinedRmsHz = -1;     // after fitDopplerOrbit (-1: not refined)
};

struct SatIdResult {
    bool ok = false;              // at least one believable candidate
    std::string why;              // when not ok
    bool confident = false;       // one candidate clearly better than all others
    bool ambiguous = false;       // several candidates fit equally well (orbit neighbours)
    double curveHz = 0;           // spread of the measured curve: with almost no Doppler in the track nothing can be told apart
    int considered = 0;           // satellites above the horizon for the whole track
    std::vector<SatIdMatch> ranked;   // best first
};

// Doppler (Hz) a satellite would impose at time t: monostatic, or bistatic when opt.bistatic. False when the propagator fails or it is below minElDeg.
bool predictDopplerHz(const Sgp4& s, const Observer& rx, double t, const SatIdOptions& opt, double& hz, double& elDeg);

SatIdResult identifySatellite(const std::vector<Tle>& tles, const Observer& rx, const std::vector<DopplerSample>& samples, const SatIdOptions& opt);

} // namespace satmisc
} // namespace dect2
