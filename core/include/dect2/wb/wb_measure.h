// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal workbench: measurements on a time range and band of a recording: the averaged spectrum, peaks, bandwidths (-3 dB, -20 dB, 99 %
// occupied), carrier offset, SNR, symbol-rate estimate (cyclostationary lines of the envelope, the FM discriminator and its steps),
// modulation hints (the signal identifier, dect2/engine/sig_ident.h), power over time and the bursts in it.
#pragma once
#include "dect2/engine/sig_ident.h"
#include "wb_dsp.h"
#include <functional>
#include <string>
#include <vector>

namespace dect2 {
namespace wb {

struct Peak { double freqHz = 0, levelDb = 0, widthHz = 0; };   // freqHz from the recording's DC; level in dBFS; width at -3 dB
struct Burst { double startSec = 0, lenSec = 0, peakDb = 0, meanDb = 0; };

struct SymEstimate {
    double baud = 0;
    double strengthDb = 0;              // how far the line stands above its surroundings
    std::string method;                 // "envelope", "frequency steps" ...
    std::vector<std::pair<double, double>> cands;   // (rate, strength dB), best first
};

struct MeasureOptions {
    int topN = 12;
    int fftSize = 8192;
    int window = WinHann;
    double minPeakDb = 8;               // peaks must stand this far above the noise floor
    bool ident = true;                  // run the signal identifier (a few seconds on a long selection)
    bool symbolRate = true;
    bool bursts = true;
};

struct Measurement {
    bool valid = false;
    std::string why;
    uint64_t t0 = 0, t1 = 0;
    double f0 = 0, f1 = 0;              // the band measured, from DC
    double rate = 0;
    bool real = false;
    // spectrum of the whole signal over the time range
    std::vector<float> psd;             // dBFS per bin
    double psdF0 = 0, binHz = 0;        // the frequency of psd[0], the bin width
    std::vector<Peak> peaks;            // strongest first, inside the band
    double noiseFloorDb = 0;            // per bin
    double peakDb = 0;                  // the strongest bin of the band
    double bandPowerDb = 0;             // total power in the band, dBFS
    double snrDb = 0;
    double bw3Hz = 0, bw20Hz = 0, obwHz = 0;
    bool bw20Limited = false;           // the -20 dB point is below the noise: bw20Hz is as wide as the noise lets it be seen
    double obwLoHz = 0, obwHiHz = 0;    // the edges of the 99 % band
    double centreHz = 0;                // the middle of the 99 % band
    double carrierHz = 0;               // the strongest line
    double carrierOffsetHz = 0;         // carrierHz - centreHz
    SymEstimate sym;
    bool identValid = false;
    sigid::Result ident;
    std::vector<float> power;           // dBFS of the band over time
    double powerStepSec = 0, powerT0Sec = 0;
    double burstThresholdDb = 0, duty = 1;
    std::vector<Burst> bursts;
};

bool measure(const Source& s, uint64_t t0, uint64_t t1, double f0, double f1, const MeasureOptions& o, Measurement& out,
             const std::function<bool(double)>& progress = nullptr);

// The pieces, for tests and for callers that have the samples already
std::vector<Peak> findPeaks(const std::vector<float>& psdDb, double f0, double binHz, int topN, double floorDb, double minOverFloorDb, int minSepBins = 4);
bool estimateSymbolRate(const cf32* x, size_t n, double rate, SymEstimate& out);
std::vector<Burst> detectBursts(const std::vector<float>& powerDb, double stepSec, double t0Sec, double& thresholdDb, double& duty);
// Rows of the peak table as CSV text
std::string peaksCsv(const Measurement& m, double centerHz = 0);

} // namespace wb
} // namespace dect2
