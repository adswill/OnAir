// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Orbit refinement from a measured Doppler curve: follow a satellite's carrier during a pass (peak tracker on the spectrum frames), then fit small
// corrections to its two-line elements (mean motion, mean anomaly, inclination, RAAN, and the receiver's frequency offset) so the SGP4 Doppler curve
// matches what was measured. Damped Gauss-Newton with a prior on each correction (a TLE is already close), so a single pass cannot throw an
// element far off. The result is DERIVED data, not an official element set: it is only as good as the receiver's frequency reference.
#pragma once
#include "dect2/geo/passes.h"
#include <string>
#include <vector>

namespace dect2 {

struct DopplerSample {
    double t = 0;        // UNIX seconds
    double hz = 0;       // measured carrier frequency minus the nominal frequency
    double sigmaHz = 0;  // 0: use the fit's default
};

// Follows the strongest narrow peak inside a window around the predicted carrier. One call per spectrum frame.
class DopplerTracker {
public:
    // dbfs: DC-centred bins as in SpectrumFrame; centerHz: tuned frequency; predictedHz: where the carrier is expected (absolute); gateHz: half window.
    // True when a peak at least minSnrDb above the window's median was found; out then holds the peak frequency minus nominalHz.
    bool feed(double t, const float* dbfs, size_t n, double centerHz, double sampleRate, double nominalHz, double predictedHz, double gateHz, DopplerSample& out) const;
    double minSnrDb = 8;
};

struct DopplerFitOptions {
    bool fitOffset = true;           // a constant frequency error of the receiver (ppm of the reference) is a fit parameter
    double offsetPriorHz = 2000;     // how far the receiver offset may reasonably be
    double priorMeanMotion = 0.002;  // rev/day, 1 sigma of the element error
    double priorMeanAnomDeg = 0.6;
    double priorInclDeg = 0.03;
    double priorRaanDeg = 0.05;
    double defaultSigmaHz = 20;
    int maxIter = 40;
    double rejectSigma = 4;          // samples further than this many sigma from the fit are dropped once and the fit repeated (0: keep all)
};

struct DopplerFit {
    bool ok = false;
    std::string why;                 // when not ok
    Tle refined;                     // the corrected elements (name marked "(derived)")
    double dMeanMotion = 0, dMeanAnomDeg = 0, dInclDeg = 0, dRaanDeg = 0, offsetHz = 0;
    double rmsBeforeHz = 0, rmsAfterHz = 0;   // residual before (with the receiver offset fitted if wanted... no: raw) and after
    double alongTrackSigmaSec = 0;   // 1 sigma timing error of the refined prediction (along-track position / orbital speed)
    double alongTrackSigmaBeforeSec = 0;   // the prior's equivalent: what the unrefined elements were assumed to be good for
    int used = 0, rejected = 0;
    std::vector<double> predictedHz; // per input sample: the refined model (same order as the input, rejected ones included)
    std::vector<double> residualHz;  // measured minus refined model
    std::vector<double> initialHz;   // measured minus the unrefined model
};

bool fitDopplerOrbit(const Tle& tle, const Observer& obs, double nominalHz, const std::vector<DopplerSample>& samples, const DopplerFitOptions& opt, DopplerFit& out);

// Two TLE lines (with checksums) for the elements. The international designator is not kept in Tle, so it is written as 00000A.
void formatTle(const Tle& t, std::string& line1, std::string& line2);
// The text to save: a comment line saying it is derived, the name, the lines.
std::string derivedTleText(const DopplerFit& f, double nominalHz);

}
