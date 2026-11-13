// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Band watch: finds the carriers on the air in the whole sample band, whether the user listed them or not, so the list can be filled
// from what is really transmitted. Spectrum of the input (300 - 600 Hz bins), peak hold with a 6 s half life, carriers = lines that stand
// 11 dB out of the noise around them and 9 dB out of the bins 4.5 - 9 kHz either side (voice sidebands and a neighbour on one side do not
// hide a carrier). The radio's tuning error is taken from the receiver when it has measured it; otherwise it is searched so that most lines
// fall on the 25 kHz raster. Each line is then moved to the nearest channel of the 8.33 kHz raster (within 450 Hz, else it is not an
// airband AM carrier and is dropped).
#pragma once
#include <memory>
#include "dect2/common/ring.h"
#include <vector>
#include "dect2/airband/airband_tel.h"

namespace dect2 {

class AirbandBandWatch {
public:
    AirbandBandWatch();
    ~AirbandBandWatch();
    void configure(double inputRateHz, double centerHz);      // forgets everything seen
    void feed(const cf32* x, size_t n);                       // never blocks
    // Looks at the held spectrum (call about twice a second); nowSec is the signal time. tuneErrHz is the receiver's measure of the radio's
    // error when tuneKnown. listedHz: channels already in the list, left out of the result.
    void update(double nowSec, double tuneErrHz, bool tuneKnown, const std::vector<double>& listedHz);
    const std::vector<AirbandSeen>& seen() const;             // most active first
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
