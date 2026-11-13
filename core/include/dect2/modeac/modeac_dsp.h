// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode A / C reply detector: complex baseband at 2 Msps or more -> replies (the thirteen information positions and the SPI position).
//
// Like the ADS-B demodulator it works on the power of the samples only, integrated over fractional windows of the running sum (so 2.4 or
// 10 samples per microsecond work the same way). A candidate is a pair of pulses 20.3 us apart (F1 and F2); its timing is refined in 0.05 us
// steps, then the fifteen pulse positions and the SPI position are measured against the weaker framing pulse. A reply must also be clean
// where it should be: the two positions after F2 and the position before F1 empty, X empty, and little energy between the pulses (a Mode S
// message, whose pulses sit on a 1 us grid, fills those gaps). Candidates that fall on a Mode S message the caller knows about are dropped
// (the mask). Processing runs behind the newest sample so that the caller's Mode S decoder has finished the messages it will find.
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <functional>
#include <vector>

namespace dect2 {

class ModeAcDemod {
public:
    struct Raw {
        double timeSec = 0;            // start of F1 on the receiver's clock (samples / rate)
        uint16_t bits = 0;             // the thirteen information positions
        bool spi = false;
        float levelDbfs = -120, snrDb = 0;
    };
    using Sink = std::function<void(const Raw&)>;
    using Mask = std::function<bool(double t0Sec, double t1Sec)>;   // true: a Mode S message was on the air in this interval

    void configure(double rate);
    void reset();                                  // forget the signal and the noise estimate; the clock keeps running
    void setSink(Sink s) { sink_ = std::move(s); }
    void setMask(Mask m) { mask_ = std::move(m); }
    void feed(const cf32* x, size_t n);
    bool ready() const { return rate_ >= 2e6 - 1; }
    double timeSec() const { return rate_ > 0 ? (double)samples_ / rate_ : 0; }
    double noiseDbfs() const;
    bool noiseReady() const { return noiseReady_; }
    uint64_t candidates() const { return candidates_; }
    uint64_t accepted() const { return accepted_; }
    uint64_t rejectedShape() const { return rejShape_; }
    uint64_t masked() const { return masked_; }
    static constexpr double kLagUs = 200.0;        // how far behind the newest sample the search runs
    void setThreshold(float k) { kPulse_ = k; }    // pulse window over the noise power (default 10, 10 dB)

private:
    double win(double x, double w) const;          // mean power in [x, x + w) (global sample position, samples)
    void scan();
    bool testAt(double s0, Raw& out);
    void compact();
    void updateNoise(float p);
    double rate_ = 0, spus_ = 0;
    Sink sink_;
    Mask mask_;
    float kPulse_ = 10.0f;
    uint64_t samples_ = 0;
    int64_t base_ = 0;                              // global index of pw_[0]
    std::vector<float> pw_;
    std::vector<double> cum_;                       // cum_[i] = sum of pw_[0 .. i-1]
    float dcRe_ = 0, dcIm_ = 0; bool dcInit_ = false;
    double scanPos_ = 0;
    double blockSum_ = 0; int blockCount_ = 0;
    float blocks_[24] = {}; int blockN_ = 0, blockPos_ = 0;
    float n0_ = 0; bool noiseReady_ = false;
    uint64_t candidates_ = 0, accepted_ = 0, rejShape_ = 0, masked_ = 0;
};

} // namespace dect2
