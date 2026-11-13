// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FM multiplex power and deviation statistics (3.9, docs/SMART5-broadcast-3.9.md item 2), the measurement of ITU-R BS.412-9 annex 2:
// the power of the whole multiplex integrated over 60 s, in dB relative to the power of a sine giving +/- 19 kHz peak deviation
// (0 dBr; the usual limit is +0 dBr). Also the peak-deviation distribution of the same minute (50, 99 and 99.9 % points, as
// ITU-R SM.1268 asks for peak deviation) and the share of samples above 75 kHz.
// push(): instantaneous deviation in Hz (the discriminator output with the DC removed), any rate.
#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 {
namespace smart5 {

struct Mpx412Report {
    bool valid = false;           // a whole integration window has been measured
    double mpxPowerDbr = -99;     // the last full 60 s window
    double runningDbr = -99;      // the window being filled (from 1 s on)
    double p50Khz = 0, p99Khz = 0, p999Khz = 0, maxKhz = 0;
    double over75Pct = 0;         // share of samples above 75 kHz
    double windowFill = 0;        // 0..1 of the current window
};

class Mpx412Meter {
public:
    explicit Mpx412Meter(double rateHz = 0, double windowSec = 60) { configure(rateHz, windowSec); }
    void configure(double rateHz, double windowSec = 60);
    void push(const float* devHz, size_t n);
    void push(float devHz) { push(&devHz, 1); }
    const Mpx412Report& report() const { return r_; }
    static double dbr(double meanSquareHz2) { return meanSquareHz2 > 0 ? 10.0 * std::log10(meanSquareHz2 / (19000.0 * 19000.0 / 2)) : -99; }
private:
    void finishWindow();
    void updateRunning();
    double rate_ = 0, win_ = 60;
    uint64_t perWin_ = 0, n_ = 0;
    double sum2_ = 0;
    static constexpr int kBins = 300;     // 0.5 kHz bins, 0 .. 150 kHz
    std::array<uint64_t, kBins> hist_{};
    uint64_t over_ = 0;
    double peak_ = 0;
    Mpx412Report r_;
};

} // namespace smart5
} // namespace dect2
