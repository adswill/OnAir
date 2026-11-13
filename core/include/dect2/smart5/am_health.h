// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AM modulation health (3.9, docs/SMART5-broadcast-3.9.md item 5): from the envelope of an AM station, second by second,
// the positive and negative modulation peaks, negative-peak clipping (the carrier pinched off: over-modulation that splatters
// onto the neighbours), carrier shift (the carrier level moving with the modulation: a weak transmitter supply or a faulty
// modulator) and dead air. push(): envelope magnitudes |x| of the channel at any rate.
#pragma once
#include <cstddef>
#include <deque>
#include <string>
#include <vector>

namespace dect2 {
namespace smart5 {

struct AmHealthReport {
    double posPeakPct = 0, negPeakPct = 0;   // the last second, 99.9 % points of the modulation
    double clipPct = 0;                      // share of samples of the last 10 s with the carrier pinched off (< 2 % of the mean)
    double carrierShiftPct = 0;              // carrier level in loud seconds against quiet seconds (over the last 60 s)
    double modRmsPct = 0;                    // the last second
    bool overModulated = false, carrierShift = false, deadAir = false;
    double deadAirFor = 0;
    std::string summary() const;
};

struct AmHealthParams { double clipLimitPct = 0.1; double shiftLimitPct = 5; double deadAirPct = 1.5; double deadAirSec = 10; };

class AmHealth {
public:
    void configure(double rateHz, AmHealthParams p = {});
    void push(const float* env, size_t n);
    const AmHealthReport& report() const { return r_; }
private:
    void endSecond();
    AmHealthParams p_;
    double rate_ = 0;
    std::vector<float> sec_;
    struct Sec { double mean, rms, clipShare; };
    std::deque<Sec> hist_;
    double quietFor_ = 0;
    AmHealthReport r_;
};

} // namespace smart5
} // namespace dect2
