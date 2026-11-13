// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// cos and sin of a phase from one 4096-point sine table with linear interpolation (error below 3e-7, about -130 dB): for the transmit
// generators that turn a frequency-modulated phase every sample, where std::cos and std::sin in double were most of the time.
#pragma once
#include "dect2/common/mathconst.h"
#include "dect2/common/ring.h"
#include <cmath>
#include <vector>

namespace dect2 {

class FastSinCos {
public:
    static const FastSinCos& get() { static const FastSinCos t; return t; }
    // amp * e^(j ph) for any finite ph (radians)
    cf32 polar(float amp, double ph) const {
        double u = ph * (1.0 / (2 * dect2::math::kPi));
        u -= std::floor(u);   // [0, 1)
        return cf32(amp * at(u + 0.25), amp * at(u));
    }
private:
    static constexpr int kN = 4096;
    std::vector<float> t_;
    FastSinCos() {
        t_.resize(kN + 2);
        for (int i = 0; i < kN + 2; i++) t_[(size_t)i] = (float)std::sin(2 * dect2::math::kPi * i / kN);
    }
    float at(double turns) const {   // sin(2 pi turns), turns in [0, 2)
        const double x = turns * kN;
        size_t i = (size_t)x;
        const float f = (float)(x - (double)i);
        i &= kN - 1;
        return t_[i] + f * (t_[i + 1] - t_[i]);
    }
};

} // namespace dect2
