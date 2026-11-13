// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Ignore DC spike": a one-pole DC blocker in front of the receiver (after the spectrum, so the display still shows the spike where it is).
// It removes the slowly varying mean of the capture, the LO leakage / DC offset that a recording carries at its centre, a few hundred Hz wide.
#pragma once
#include <algorithm>
#include <complex>
#include <cstddef>
namespace dect2 {
class DcNotch {
public:
    void set(double rateHz, double widthHz = 400.0) {
        a_ = rateHz > 0 ? (float)std::min(0.5, 6.2831853 * widthHz / rateHz) : 0.f;
    }
    void reset() { m_ = {0.f, 0.f}; primed_ = false; }
    void process(std::complex<float>* x, size_t n) {
        if (!n) return;
        if (!primed_) { m_ = x[0]; primed_ = true; }   // starts on the first sample: no click while the mean is learned
        for (size_t i = 0; i < n; i++) {
            m_ += a_ * (x[i] - m_);
            x[i] -= m_;
        }
    }
    std::complex<float> mean() const { return m_; }
private:
    float a_ = 0.f;
    std::complex<float> m_{0.f, 0.f};
    bool primed_ = false;
};
}
