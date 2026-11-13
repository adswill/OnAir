// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Radio clock from GNSS (3.9, docs/SMART5-gnss-3.9.md item 21). A GNSS position fix also solves the receiver's clock drift
// (s/s). When the samples come from the SDR, that drift is the SDR's reference-oscillator error: a 2 ppm crystal shows as a drift
// of 2e-6 s/s. This smooths the drift from every fix into a ppm figure with its uncertainty and its trend (warm-up), and gives
// the correction in Hz at any tuned frequency, so every other mode can start on frequency.
// Sign: a positive drift (the receiver clock gains) means the oscillator runs fast; a fast oscillator tunes high, so a signal at
// f appears lower by f x ppm x 1e-6 and the correction to add is +f x ppm x 1e-6 to the tuned frequency.
#pragma once
#include <cmath>
#include <deque>
#include <string>

namespace dect2 {
namespace smart5 {

class GnssClockCal {
public:
    // drift: the PVT solution's clock drift in s/s (or m/s divided by c); valid false for fixes without a drift solution
    void fix(double t, double driftSps, bool valid = true, int nSats = 4);
    bool ready() const { return n_ >= 10; }
    double ppm() const { return ppm_; }
    double sigmaPpm() const { return sigma_; }
    double trendPpmPerMin() const { return trend_; }
    bool settled() const { return ready() && std::abs(trend_) < 0.005; }   // under 0.005 ppm / min: warm-up done
    double correctionHz(double tunedHz) const { return tunedHz * ppm_ * 1e-6; }
    std::string text() const;
private:
    std::deque<std::pair<double, double>> w_;   // last 120 valid fixes (t, ppm)
    double ppm_ = 0, sigma_ = 0, trend_ = 0;
    int n_ = 0;
};

} // namespace smart5
} // namespace dect2
