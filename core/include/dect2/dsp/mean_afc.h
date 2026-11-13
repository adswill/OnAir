// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Frequency capture for the 4-level FSK receivers (P25, NXDN): the mean of the FM discriminator output over 20 ms, taken only while a carrier with
// a modulation-sized spread is there (the spread of noise is several times larger), is the carrier's distance from the oscillator. The balanced
// symbols of a scrambled or coded frame average to nothing, so the mean is the tuning error; the receiver moves its oscillator by it. Only errors beyond what the receivers' own per-frame offset estimate
// follows (4.5 kHz) are corrected: a smaller move would only disturb the first call.
//
// Before this the receivers relied on the channel filter alone: a carrier more than about 4.5 kHz off was cut by the filter and nothing decoded,
// although a radio without a good reference is 5 kHz off on the 800 MHz band (6 ppm) and a handset can be 2.5 ppm off on its own.
#pragma once
#include <cmath>

namespace dect2 {

class MeanAfc {
public:
    // f: the discriminator output (Hz) at `rate` samples per second. Returns the correction in Hz when one is due, else 0.
    double push(float f, double rate) {
        if (hold_ > 0) { hold_--; return 0; }
        s_ += f; q_ += (double)f * f;
        if (++n_ < (int)(0.02 * rate)) return 0;
        const double m = s_ / n_, var = q_ / n_ - m * m;
        n_ = 0; s_ = q_ = 0;
        if (var > kMaxSpreadHz * kMaxSpreadHz) { fn_ = 0; fs_ = 0; return 0; }
        if (std::fabs(m) < kMinHz) {
            // the narrow-channel modes (6.25 kHz NXDN, D-STAR): an error of 1 to 4.5 kHz already cuts into the channel filter and costs several dB,
            // so a mean that holds over four windows (80 ms) is followed too
            if (!fine_) return 0;
            fs_ += m;
            if (++fn_ < 4) return 0;
            const double M = fs_ / fn_;
            fn_ = 0; fs_ = 0;
            if (std::fabs(M) < fineMin_) return 0;
            hold_ = (int)(0.012 * rate);
            return M;
        }
        fn_ = 0; fs_ = 0;
        hold_ = (int)(0.008 * rate);        // the channel filter's delay: the next window sees the moved signal
        return m;
    }
    void reset() { n_ = 0; s_ = q_ = 0; hold_ = 0; fn_ = 0; fs_ = 0; }
    // minHz: the smallest mean error that is followed (receivers that move only while searching can use a small one)
    void enableFine(bool on = true, double minHz = kFineHz) { fine_ = on; fineMin_ = minHz; }
    static constexpr double kMaxSpreadHz = 3500.0, kMinHz = 4700.0, kFineHz = 2500.0, kMaxTotalHz = 7500.0;
private:
    int n_ = 0, hold_ = 0, fn_ = 0;
    double s_ = 0, q_ = 0, fs_ = 0;
    bool fine_ = false;
    double fineMin_ = kFineHz;
};

} // namespace dect2
