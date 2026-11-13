// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF beacon detection, the processing behind HfbReceiver; usable on its own (tests).
//
// Stage 1 is a Hann-windowed FFT of N samples without overlap (N the largest power of two <= rate / 1500): bin k of successive frames is the
// signal around fc + k * rate / N at 1500 .. 3000 Hz. Stage 2 takes 32 of those samples per watched bin, hops 16, and makes a 32 point
// spectrum (47 - 76 Hz bins, 6 - 11 ms steps). For each beacon the power of the three bins at its frequency is compared with the median of the 64
// bins (the local noise floor): x = (S3 - 3 Nb) / Nb, and SNR(100 Hz) = x * gain / 32 * rate1 / 100, gain = 1.5 / (the Hann passband shape at the tone's place in the bin).
// Beacons of one kind are then told from random carriers by their keying:
//  - NCDXF/IARU: the 10 s slot of the band gives the beacon (hf_beacon_list.h); the callsign must be keyed (>= 4 elements) and the first dash
//    (100 W, about 1 s) a run of 0.75 .. 1.4 s; the four dash windows give the power steps.
//  - single letters: the on/off runs are fitted to the Morse of the marker (unit from the short gaps), at least two clean repeats.
//  - Long Dashes: runs of 2.2 .. 3.6 s repeating at the 27 s cycle (or agreeing with another frequency's dash in the 3 s ladder).
//  - carriers (user entries): steady energy.
#pragma once
#include "hfbeacon_list.h"
#include "hfbeacon_tel.h"
#include "dect2/common/ring.h"
#include <memory>
#include <string>
#include <vector>

namespace dect2::hfb {

class Analyzer {
public:
    Analyzer();
    ~Analyzer();
    void setBeacons(std::vector<Beacon> b);               // the stats of beacons that stay (same name and frequency) are kept
    // restart the signal processing (a retune, a new rate): the stats, histories and the clock settings stay
    void configure(double rateHz, double centerHz, double originUtc, bool clockValid);
    void setPpm(double ppm);                              // a signal at f is looked for at f * (1 + ppm * 1e-6)
    void setClockOffset(double sec);                      // the computer clock is `sec` ahead of true UTC
    void setObserver(bool have, double lat, double lon);
    void setSelected(int index);
    void feed(const cf32* x, size_t n);
    void fill(HfbTelemetry& t) const;
    void clear();                                         // the stats and histories
    std::string saveResults() const;
    bool loadResults(const std::string& blob);
    const std::vector<Beacon>& beacons() const;
    double rate1() const;                                 // the stage 1 output rate, Hz
    std::string debugRuns(int beacon) const;              // the channel's latest on / off runs, for diagnosis
    uint64_t falseRuns() const;                           // runs started in channels with no beacon of the right kind active (tests)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// the Morse of a marker letter as dots and dashes ("-.." for D)
const char* morseOf(char letter);
// SNR in 100 Hz from the mean excess x (in noise bins) at the stage 2 settings
double snrFromExcess(double x, double rate1, double gain = 1.5);

} // namespace dect2::hfb
