// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cell towers: small signal-processing helpers shared by the LTE, NR and GSM parts.
#pragma once
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2::cell {

// Arbitrary-ratio resampler (windowed sinc, table driven). Output sample n is taken at input position start + n * step (in input samples),
// after the input has been mixed down by shiftHz (relative to fsIn) and low-passed to cutoff (fraction of fsIn, 0 .. 0.5). Positions outside
// the input give 0.
class Resampler {
public:
    explicit Resampler(double cutoff, int halfLen = 0);   // halfLen 0: chosen from the cutoff
    void run(const cf32* x, size_t n, double start, double step, cf32* out, size_t nOut) const;
    int halfLen() const { return H_; }
private:
    double fc_;
    int H_;
    std::vector<float> tab_;   // the filter at 1/kOver input-sample steps over [-H, H]
    std::vector<float> bank_;  // the same taps phase by phase: row q (0..kOver) holds the 2H taps of fraction q / kOver, oldest input first
    static constexpr int kOver = 128;
};

// Mix x by -hz (relative to rate) in place, starting with phase phase0 (radians); returns the phase after the last sample
double mixDown(cf32* x, size_t n, double hz, double rate, double phase0 = 0);

// Resample a whole block: in at fsIn mixed down by shiftHz, out at fsOut over [0, nOut) output samples starting at input position start,
// with step fsIn/fsOut * clockScale; the low-pass at bwHz/2 (one-sided)
std::vector<cf32> resampleBlock(const cf32* x, size_t n, double fsIn, double fsOut, double shiftHz, double bwHz, double start, size_t nOut, double clockScale = 1.0);

// Power spectrum (Welch, Hann) of nfft bins, fftshifted (bin 0 = -fs/2), linear power per bin
std::vector<float> welch(const cf32* x, size_t n, int nfft, int maxAvg = 400);

// next power of two >= n
int pow2At(int n);

} // namespace dect2::cell
