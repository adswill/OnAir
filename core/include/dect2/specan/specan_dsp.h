// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Spectrum analyser building blocks: the sweep grid and the stitching of FFT snapshots onto it, spur removal at the tuning steps,
// the live / peak-hold / averaged traces, CSV export and a small table of European frequency allocations. No radio, no threads.
#pragma once
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <functional>
#include <string>
#include <vector>

namespace dect2 {

// The output grid: bins of binHz from loHz upwards (bin k covers loHz + k * binHz .. + binHz)
struct SaGrid {
    double loHz = 0, binHz = 1e5;
    size_t bins = 0;
    double centre(size_t k) const { return loHz + (k + 0.5) * binHz; }
    long index(double hz) const { return hz < loHz ? -1 : (long)((hz - loHz) / binHz); }   // may be >= bins
};
SaGrid saMakeGrid(double loHz, double hiHz, double binHz, size_t maxBins = 4000000);   // binHz is widened only when the range needs more than maxBins (6 GHz at 2.8 kHz = 2.1 M)

// Puts the power spectra of the tuning steps together into one sweep.
class SaStitcher {
public:
    void setGrid(const SaGrid& g);
    const SaGrid& grid() const { return g_; }
    // One FFT power spectrum (linear power, fftshifted: index 0 = loFreq - rate / 2) taken with the radio at loFreqHz. Only the part with
    // dcGapHz <= |f - lo| <= usableHz goes onto the grid; each grid bin is the mean power of the FFT bins inside it (or the nearest one).
    void add(double loFreqHz, double rateHz, const float* pow, size_t n, double usableHz, double dcGapHz);
    void beginSweep();                     // forget the steps so far
    std::vector<float> finishDb() const;   // dB per grid bin; bins no step covered: -200
    size_t covered() const;                // grid bins with data in this sweep
    const std::vector<double>& stepLos() const { return los_; }   // the radio's centres in this sweep (for spur removal)
private:
    SaGrid g_;
    std::vector<double> acc_;
    std::vector<uint32_t> cnt_;
    std::vector<double> los_;
};

// The radio's own artefacts at each step centre (the DC spike / LO leak): a bin within widthHz of a step centre that stands more than
// thresholdDb over the median of its neighbours just outside that width is set to that median. Returns the bins changed.
int saRemoveSpurs(std::vector<float>& db, const SaGrid& g, const std::vector<double>& stepLos, double widthHz, double thresholdDb = 3);

// Live, peak-hold and averaged traces (dB in, dB out; the average is taken in power). avgN 1 = no averaging.
struct SaTraces {
    std::vector<float> live, peak, avg;
    int avgN = 8;
    uint64_t sweeps = 0;
    void reset() { live.clear(); peak.clear(); avg.clear(); sweeps = 0; avgPow_.clear(); }
    void resetPeak() { peak = live; }
    void update(const std::vector<float>& db);
private:
    std::vector<double> avgPow_;
};

// CSV: "frequency_hz,live_dbfs,peak_dbfs,average_dbfs" then one row per bin (empty columns when a trace is off / missing)
std::string saCsv(const SaGrid& g, const std::vector<float>& live, const std::vector<float>& peak, const std::vector<float>& avg);
// The same CSV streamed in chunks through sink (no giant string); returns the rows written (= g.bins)
size_t saCsvStream(const SaGrid& g, const std::vector<float>& live, const std::vector<float>& peak, const std::vector<float>& avg,
                   const std::function<bool(const char*, size_t)>& sink);
bool saCsvWriteFile(const std::string& path, const SaGrid& g, const std::vector<float>& live, const std::vector<float>& peak, const std::vector<float>& avg);
size_t saCsvEstimateBytes(size_t bins);   // about what the file will take

struct SaBand { double loMhz, hiMhz; const char* name; const char* use; };
const SaBand* saAllocations(int& count);          // EU allocations (ECA / ERC Rec 70-03 summary), sorted by loMhz
const SaBand* saAllocationAt(double mhz);         // the narrowest band holding mhz, nullptr none

struct SaPreset { const char* name; double loMhz, hiMhz; };
const SaPreset* saPresets(int& count);

// "Nice" axis ticks: a step of 1, 2 or 5 x 10^n giving about maxTicks over lo..hi
double saTickStep(double lo, double hi, int maxTicks);

// The HackRF's native sweep hands blocks of samples tagged with their tuning; the source posts them here and the receiver takes them.
struct SaBlock { double loHz = 0; double rateHz = 0; std::vector<cf32> iq; };
class SaSweepTap {
public:
    static SaSweepTap& get();
    void setActive(bool on, double rateHz = 0, double offsetHz = 0);   // the source in sweep mode: the rate and the LO offset over each block's tag
    bool active() const;
    double offsetHz() const;
    void post(double tagHz, const cf32* x, size_t n);   // tag: the frequency the block header gives (LO = tag + offset)
    bool take(std::vector<SaBlock>& out);              // everything posted so far
private:
    mutable std::mutex mu_;
    bool on_ = false;
    double rate_ = 0, off_ = 0;
    std::vector<SaBlock> q_;
};

} // namespace dect2
