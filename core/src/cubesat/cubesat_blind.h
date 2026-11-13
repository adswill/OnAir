// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Blind analysis of the Smart CubeSat decoder (internal): the narrow-carrier scan of a wide capture and the classifier of one carrier.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/cubesat/cubesat_chain.h"
#include "dect2/cubesat/cubesat_db.h"
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
class Fft;
namespace cubesat {

struct ScanPeak {
    double hz = 0;               // relative to the centre of the span
    double widthHz = 0;
    double snrDb = 0;            // above the noise floor, in the carrier's own width
    double peakDb = 0;           // the highest bin over the noise floor
    double flatDb = 0;           // spread of the bins inside the group (a chirp signal is flat)
};

struct ScanCarrier {
    int id = 0;
    double hz = 0, widthHz = 0, snrDb = 0, peakDb = 0, flatDb = 0;
    int hits = 0;
    bool active = false;         // seen in the last estimate
    double ageSec = 0;
};

// Narrow carriers in a wide capture: Hann-windowed spectra averaged over about 0.25 s, the noise floor from the lowest 30 % of each segment of
// the span, groups of bins above it, merged, tracked from estimate to estimate.
class CarrierScan {
public:
    double minSnrDb = 8.0;
    void configure(double rateHz, double binHz = 0);
    void reset();
    void feed(const cf32* x, size_t n);
    std::vector<ScanCarrier> carriers() const;            // those seen in at least three estimates
    double noiseDb() const { return noiseDb_; }
    uint64_t updates() const { return updates_; }
    double binHz() const { return rate_ / (double)n_; }
private:
    void update();
    struct Cand { int id = 0; ScanPeak p; int hits = 0, misses = 0; bool seenNow = false; };
    double rate_ = 0, noiseDb_ = -200;
    size_t n_ = 4096, hop_ = 4096, skip_ = 0;
    int target_ = 1, nAvg_ = 0, fill_ = 0, nextId_ = 1;
    uint64_t updates_ = 0;
    std::shared_ptr<Fft> fft_;
    std::vector<float> win_;
    std::vector<double> acc_;
    std::vector<cf32> buf_;
    std::vector<Cand> cands_;
};

struct Hyp { int mod = 0; int baud = 0; double score = 0; };

struct ProbeResult {
    double bwHz = 0, tonality = 0, envCv = 0, offsetHz = 0;
    double afskDb = -100, bpskDb = -100, mskDb = -100, mskSepHz = 0, fskDb = -100, bpskCycDb = -100, bpskLineHz = 0;
    double estBaudFsk = 0, estBaudBpsk = 0;
    bool carrierOnly = false;
    std::vector<Hyp> hyps;       // best first
    std::string note;
};

// 1.5 s or more of one carrier at 96 kHz. forceMod != ModUnknown: only that hypothesis (forceBaud 0: all usual baud rates of it are the caller's job)
ProbeResult classifyCarrier(const std::vector<cf32>& y, int forceMod = ModUnknown, int forceBaud = 0);
std::string modulationText(int mod, int baud);

} // namespace cubesat
} // namespace dect2
