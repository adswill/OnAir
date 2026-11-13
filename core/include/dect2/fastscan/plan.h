// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Planning for the fast scanner: per-radio capture profiles (rate, usable fraction, retune cost), the hop plan for a set of bands with
// exclusion ranges and priority bands, the speed model (the retune-limited ceiling of a radio) and channelisation of a spectrum into
// channels. Pure code.
#pragma once
#include "dect2/fastscan/hits.h"
#include <string>
#include <vector>

namespace dect2 {
namespace fastscan {

// How one kind of radio is driven for a scan. retuneSec is the typical time from the tune command to a clean spectrum (tuner PLL lock plus the
// samples of the old frequency still in the USB pipeline); the real value is measured at the start of a scan (sweep::calibrateSettle) and
// replaces it. These numbers are planning values, not measurements of a particular unit.
struct RadioProfile {
    std::string id, name;
    double rateHz = 2.4e6;          // capture rate used for scanning
    double usableFrac = 0.70;       // fraction of the rate kept per hop (the rest is filter roll-off at the edges)
    double retuneSec = 0.012;       // typical retune + settle
    double minDwellSec = 0.002;     // shortest useful capture per hop
    bool nativeSweep = false;       // the radio's firmware can sweep by itself (HackRF): the retune cost per hop is then a few hundred microseconds
    double nativeRetuneSec = 0.0004;
    double dcFrac = 0.0;            // width of the DC spike region as a fraction of the rate (masked); 0: the radio has none (zero-IF has one)
};

const std::vector<RadioProfile>& radioProfiles();
const RadioProfile& radioProfile(const std::string& id);                 // unknown id: the first (RTL-SDR)
// The profile for a radio by the board string / name DeviceInfo carries and the fastest rate it offers (0: unknown)
RadioProfile profileForBoard(const std::string& board, double maxRateHz);

struct FastPlanConfig {
    std::vector<FreqRange> bands;        // what to scan (Hz); several are stitched on one grid
    std::vector<FreqRange> exclusions;   // known interferers, FM broadcast: hops wholly inside are skipped, bins inside are masked
    std::vector<FreqRange> priority;     // bands visited more often
    int priorityEvery = 4;               // one priority hop after this many ordinary ones
    double binHz = 12500;                // wanted bin width
    double dwellSec = 0.004;             // capture per hop after the settle time
    double settleSec = -1;               // < 0: the profile's
    double cropFrac = -1;                // < 0: derived from the profile's usable fraction
};

struct Hop {
    double centerHz = 0, loHz = 0, hiHz = 0;   // usable span of the hop
    size_t firstBin = 0, nBins = 0;            // its place on the stitched grid
    bool priority = false;
};

struct FastPlan {
    double startHz = 0, binHz = 0, rateHz = 0;
    int fftSize = 0, averages = 1;
    size_t binsPerHop = 0, totalBins = 0, samplesPerHop = 0;
    double settleSec = 0, dwellSec = 0;
    std::vector<Hop> hops;
    std::vector<int> order;              // hop indexes for one pass, priority hops interleaved
    std::vector<uint8_t> mask;           // totalBins: non-zero = excluded bin
    double passSec = 0;                  // estimated length of a pass (hops x (settle + dwell))
    double coveredHz = 0;                // the width actually scanned (exclusions taken out)
    double mhzPerSec = 0;                // covered MHz per second at that pass length
    double frac(double hz) const { return (hz - startHz) / binHz; }
    long binOf(double hz) const;         // -1 outside
};
FastPlan planFast(const FastPlanConfig& c, const RadioProfile& r);

// The speed a radio can reach if the signal processing is free: usable MHz per hop over (retune + max(dwell, one FFT)). With nativeSweep the
// retune is the firmware's. Used for the "retune-limited ceiling" line in the UI and the docs.
double ceilingMhzPerSec(const RadioProfile& r, double dwellSec);

// Channelisation: the power of channels of widthHz on a raster (origin originHz) from the spectrum in dB. Per channel: the mean power of the
// bins whose centres lie inside it (dB), the strongest bin and its margin over the floor.
struct ChannelPower { double centerHz = 0; float meanDb = -150, peakDb = -150, snrDb = 0; int bins = 0; };
std::vector<ChannelPower> channelise(const float* db, const float* floorDb, size_t n, double firstHz, double binHz, double originHz, double widthHz,
                                     double loHz, double hiHz);

// Mode guess for a hit with no memory entry: from the frequency (airband AM, broadcast FM, everything else narrow FM). Returns the scanner's mode keys.
std::string guessMode(double freqHz, double bwHz);

}  // namespace fastscan
}  // namespace dect2
