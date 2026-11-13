// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ionospheric chirp sounders (ionosondes, oblique sounders), the processing behind IonoReceiver; usable on its own (tests).
//
// A sounder sweeps 0 .. 30 MHz linearly at a known rate (100 kHz/s most often, also 125, 150, 200, 500 kHz/s) and starts each sweep at
// a known offset in its repetition period. Detection: a short Hann FFT (about 3 ms, the best resolution for a chirp of 100 kHz/s) of the
// captured span; each strong bin at RF frequency f seen at time t says "a sweep of rate r passed 0 MHz at t - f / r". Hits of every
// candidate rate are counted per 10 ms of that start time; a start time that keeps getting hits for long enough (a wrong rate drifts away
// within a few tenths of a second) is a sweep. Its start is rounded to the whole second when the timing is locked (sounders run from GPS).
// Ionogram: from then on the samples are dechirped (multiplied by the conjugate of the model chirp), so an echo delayed by tau becomes a
// tone at -r tau; a boxcar decimation and an FFT over 20 kHz of sweep give the delay profile, accumulated per 50 kHz column of the
// sounder's ionogram (delay 0 .. 10 ms in 0.05 ms bins; group range c tau). Once the period is known the next sweeps are expected and
// dechirped from the moment they enter the span.
// Scaling: at the end of each sweep, the columns it crossed replace those of the sounder's "newest" ionogram, which is scaled
// (iono_scale.h): the traces, the values (IonoSounder::scale) and a history point per sweep that gave echo traces.
#pragma once
#include "iono_tel.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace iono {

constexpr double kC = 299792.458;            // km/s
constexpr int kNumRates = 5;
constexpr double kRates[kNumRates] = {100e3, 125e3, 150e3, 200e3, 500e3};   // Hz/s
constexpr double kBandHz = 30e6;             // what a sweep covers
constexpr double kColHz = 50e3;
constexpr int kCols = 600;                   // 0 .. 30 MHz
constexpr double kDelayBin = 50e-6;          // s (15 km of group range)
constexpr int kRows = 200;                   // 0 .. 10 ms
constexpr double kEdge = 0.45;               // the usable span: +-0.45 x the sample rate around the centre

inline double groupRangeKm(double delaySec) { return kC * delaySec; }
inline double rowDelay(int row) { return row * kDelayBin; }

class Processor {
public:
    Processor();
    ~Processor();
    // fs: sample rate; centerHz: the radio's frequency; originSec: the time of the first sample (UTC seconds with a radio, 0 for a file)
    void configure(double fs, double centerHz, double originSec);
    void setCenterHz(double hz);         // after a retune (the coverage follows)
    void setLocked(bool on);             // sweep starts on whole seconds (default on)
    void setSelected(int index);         // the sounder whose ionogram fill() hands out (-1: the latest one heard)
    void setObserver(bool have, double lat, double lon);   // the receiver location (distances of known sounders, iono_scale.h)
    void clear();                        // forget the sounders, ionograms and coverage
    // configure() keeps the sounders heard, their history and the sweep timeline (only clear() forgets them). The results file holds the
    // sounders with their history and the timeline (not the ionogram sums: a loaded sounder starts a fresh ionogram).
    std::string saveResults() const;
    bool loadResults(const std::string& blob);
    void feed(const cf32* x, size_t n);
    void fill(IonoTelemetry& t) const;   // everything but seq
    double now() const;
    int detectorFft() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The selected sounder's ionogram as CSV (frequency MHz, delay ms, group range km, dB), one line per cell with data
std::string ionogramCsv(const IonoTelemetry& t);

} // namespace iono
} // namespace dect2
