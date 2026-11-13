// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LNB oscillator calibration from a known beacon, and a slow drift tracker. Pure functions and a small filter: no radio, no threads.
#pragma once

namespace dect2 {

struct LnbError {
    double hz = 0;      // actual LO minus nominal LO (Hz); positive when the LNB runs high
    double ppm = 0;     // the same as parts per million of the nominal LO
};

// rfHz: the beacon's known frequency at the feed. loNominalHz: the LO the radio assumes. measuredIfHz: the beacon's measured IF peak
// (fine FFT peak with parabolic interpolation). inverted: the LO is above the downlink (IF = LO - RF), as for 10600 MHz presets.
LnbError lnbErrorFromBeacon(double rfHz, double loNominalHz, double measuredIfHz, bool inverted);

// The LO to set on the tuner so that the IF stays right: nominal LO corrected by a stored ppm error.
double lnbCorrectedLoHz(double loNominalHz, double ppm);

// Slow filter over repeated ppm measurements. The first sample starts it; the ppm estimate is an exponential average with a time
// constant tauSec, and ppmPerHour is the slope of that average (the drift), also averaged. Times in seconds, any increasing clock.
class LnbDriftTracker {
public:
    explicit LnbDriftTracker(double tauSec = 1800.0);
    void reset();
    void add(double tSec, double ppm);     // samples with tSec not after the last one are ignored
    bool valid() const { return n_ > 0; }
    int samples() const { return n_; }
    double ppm() const { return ppm_; }             // filtered LNB error, ppm
    double ppmPerHour() const { return rate_; }     // filtered drift, ppm per hour (positive = the LNB runs higher over time)
private:
    double tau_;
    int n_ = 0;
    double t_ = 0, ppm_ = 0, rate_ = 0;
};

} // namespace dect2
