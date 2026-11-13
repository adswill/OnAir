// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Streaming resampler for any ratio (windowed sinc, finely interpolated phases, position tracked in double precision), for receivers whose
// sample rate is not a small rational multiple of the radio's (a rational approximation is tens of ppm off at 10 Msps).
#pragma once
#include "dect2/common/ring.h"
#include <memory>
#include <vector>

namespace dect2 {

// The resampling loop shared by the resamplers: one windowed-sinc dot product per output sample (AVX2 where the CPU has it). R and M are the
// real and imaginary input rows, B the filter bank ((phases + 1) rows of stride taps, stride a multiple of 8), pos the position of the next output
// in input samples (not negative); writes to o, which must have room, and returns the count.
size_t resampleLoop(const float* R, const float* M, const float* B, int stride, int phases, int hm1, long avail, double& pos, double step, cf32* o);

class ExactResampler {
public:
    // Returns false for ratios the filter cannot do (the output rate must not exceed about 4 times the input rate).
    bool configure(double inRate, double outRate, int lobes = 16);   // lobes: half the filter length in input samples at equal rates (16: about 90 dB, 8: about 55 dB and half the cost)
    bool passthrough() const { return pass_; }
    void reset();
    // Lets the output run slower (factor > 1: every output sample advances that much further in the input) or faster; for tracking a clock error.
    void scaleStep(double factor) { step_ *= factor; }
    void setStep(double s) { step_ = s; }   // (back to a step read with step(), e.g. the nominal one after a restart)
    double step() const { return step_; }
    // Appends resampled samples to `out`.
    void process(const cf32* in, size_t n, std::vector<cf32>& out);

private:
    bool pass_ = true;
    double step_ = 1.0;        // input samples per output sample
    int half_ = 16, taps_ = 32, stride_ = 32, phases_ = 2048;
    double down_ = 1.0;
    std::shared_ptr<const std::vector<float>> bank_;  // (phases + 1) x stride, built on first use (ensureBank), shared by equal ratios
    void ensureBank();
    std::vector<float> re_, im_;   // buffered input (with history)
    double pos_ = 0;           // position of the next output sample, in input samples counted from re_[0]
};

} // namespace dect2
