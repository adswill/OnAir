// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Channeliser for several adjacent TV channels in one wide capture: every channel is mixed to baseband, low-passed and resampled to the rate
// its receiver wants (ChannelFilter), the channel slots of the span come from the channel raster (spanSlots), and SpanScanner measures which
// slots carry a signal (an averaged power spectrum, the noise floor from the quietest part of the span).
#pragma once
#include "dect2/dsp/exact_resampler.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <vector>

namespace dect2 {

// One channel of a wide stream: offsetHz from the span centre mixed to 0, then resampled to outRate (windowed sinc, exact ratio).
// The mixer is a 4096-entry table turned by a phase per period, in plain float arithmetic that the compiler vectorises.
class ChannelFilter {
public:
    bool configure(double inRate, double offsetHz, double outRate);
    void reset();
    // appends the channel's samples to out
    void process(const cf32* x, size_t n, std::vector<cf32>& out);
    double offsetHz() const { return shift_; }
    double outRate() const { return outRate_; }

private:
    static constexpr size_t L = 4096;
    double inRate_ = 0, outRate_ = 0, shift_ = 0;
    std::vector<float> tre_, tim_;
    double phRe_ = 1, phIm_ = 0, stepRe_ = 1, stepIm_ = 0;
    size_t pos_ = 0;
    std::vector<cf32> mix_;
    ExactResampler rs_;
};

// The rate a TV receiver is fed at for a channel of widthMhz: 1.25 times the width (a little room for the filter's transition and the receiver's
// own resampler), never more than the span's rate
double tvChannelRate(double widthMhz, double spanRate);

struct ChannelSlotPos {
    int index = 0;           // position on the raster, 0 = the raster's origin
    double offsetHz = 0;     // centre of the slot relative to the centre of the span
};

// The slots of the raster (origin at gridOffsetHz from the span centre, spacing widthHz) whose whole width lies within edgeFrac of the span's rate
// (the converter's filter rolls off before the span's edge). Sorted by frequency.
std::vector<ChannelSlotPos> spanSlots(double spanRate, double widthHz, double gridOffsetHz, double edgeFrac = 0.46);

struct SlotPower {
    ChannelSlotPos pos;
    double levelDb = -200;   // median of the slot's centre 90 % in the averaged spectrum (dB, relative to full scale)
    double aboveDb = 0;      // over the quietest part of the span
    bool occupied = false;
};

class SpanScanner {
public:
    void configure(double spanRate);
    void reset();
    void feed(const cf32* x, size_t n);
    size_t frames() const { return nFrames_; }
    // slots measured against the noise floor; occupied when aboveDb >= thresholdDb
    std::vector<SlotPower> measure(const std::vector<ChannelSlotPos>& slots, double widthHz, double thresholdDb = 6.0) const;
    // the quietest 250 kHz window within the usable part of the span (dB)
    double floorDb(double edgeFrac = 0.46) const;
    double binHz() const { return rate_ / kN; }
    // the averaged spectrum in dB, DC in the middle (kN bins); empty before the first frame
    std::vector<float> spectrumDb() const { if (!nFrames_) return {}; refresh(); return db_; }

private:
    static constexpr int kN = 4096;
    double rate_ = 0;
    std::vector<float> acc_;     // averaged power per bin, DC in the middle
    std::vector<float> win_;
    std::vector<cf32> blk_;
    size_t fill_ = 0, nFrames_ = 0, blocks_ = 0;
    mutable bool dirty_ = true;
    mutable std::vector<float> db_;
    void refresh() const;
};

} // namespace dect2
