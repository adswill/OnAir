// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Whole-span decoding (the "wide decode" switch): the visible span of the capture, not one transmission.
// WideDecimator mixes the dial frequency to 0 Hz and brings the span (clamped to kMaxSpanHz) down to a rate of 1.5 x the span (at least
// 260 kHz), with a windowed-sinc FIR that is only evaluated at the output samples. The narrowband receivers (CW, PSK, HF digital,
// RTTY) then run one instance per slice of the span on that stream, each with setSignalOffset(slice centre), and merge their results.
#pragma once
#include "dect2/common/ring.h"
#include <cstddef>
#include <vector>

namespace dect2 {

constexpr double kWideMaxSpanHz = 250000.0;   // the default span cap: one decoder per slice, so a wider span costs more processor time

// The cap on the span of whole-span decoding is a default, not a limit (3.6): the app lifts it when the user chose a custom bandwidth (the user
// decides; a wide span only costs processor time). wideSpanCapHz() is kWideMaxSpanHz until setWideSpanCapHz() says otherwise (<= 0: the default).
double wideSpanCapHz();
void setWideSpanCapHz(double hz);

class WideDecimator {
public:
    // false when the input is too slow to be split (< 260 kHz): then outRate() == inRate and the span is clamped to 80 % of it
    void configure(double inRate, double spanHz);
    void setOffsetHz(double hz);           // where the dial sits in the input (mixed to 0)
    void reset();
    void process(const cf32* x, size_t n, std::vector<cf32>& out);   // appends
    double outRate() const { return outRate_; }
    double spanHz() const { return span_; }
    int decimation() const { return dec_; }
private:
    double fs_ = 0, outRate_ = 0, span_ = 0, off_ = 0;
    int dec_ = 1;
    std::vector<float> h_;
    std::vector<cf32> buf_;
    size_t pos_ = 0;
    cf32 rot_{1.f, 0.f}, step_{1.f, 0.f};
    int rotCount_ = 0;
};

} // namespace dect2
