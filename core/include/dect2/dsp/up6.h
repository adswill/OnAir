// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Voice audio 8 kHz -> 48 kHz: a polyphase interpolator, 96-tap Blackman-windowed low-pass at 3.8 kHz (16 taps per output phase). The straight-line
// interpolation it replaces left the images of the speech above 4 kHz only about 13 dB down (a 3 kHz component came back at 5 kHz): the voice
// sounded sibilant and metallic (the same was heard and fixed in XMC_SDR). Delay: 47.5 samples at 48 kHz, about 1 ms.
#pragma once
#include <cmath>

namespace dect2 {

class Up6 {
public:
    static constexpr int kUp = 6, kPerPhase = 16, kTaps = kUp * kPerPhase;
    Up6() {
        const double pi = 3.14159265358979323846, fc = 3800.0 / 48000.0;
        double h[kTaps], sum = 0;
        for (int n = 0; n < kTaps; n++) {
            const double t = n - (kTaps - 1) / 2.0;
            const double s = std::fabs(t) < 1e-9 ? 2 * fc : std::sin(2 * pi * fc * t) / (pi * t);
            const double w = 0.42 - 0.5 * std::cos(2 * pi * n / (kTaps - 1)) + 0.08 * std::cos(4 * pi * n / (kTaps - 1));
            h[n] = s * w;
            sum += h[n];
        }
        for (int p = 0; p < kUp; p++)
            for (int k = 0; k < kPerPhase; k++) ph_[p][k] = (float)(h[p + kUp * k] * kUp / sum);   // each phase passes DC at gain 1
        reset();
    }
    void reset() { for (float& v : hist_) v = 0; pos_ = 0; }
    // one 8 kHz sample in, six 48 kHz samples out (out(v) called for each, oldest first)
    template <class Out> void push(float s, Out&& out) {
        hist_[pos_] = hist_[pos_ + kPerPhase] = s;   // twice: the newest kPerPhase are always in one run
        pos_ = (pos_ + 1) % kPerPhase;
        const float* x = hist_ + pos_;               // oldest first; x[kPerPhase - 1] is s
        for (int p = 0; p < kUp; p++) {
            float acc = 0;
            for (int k = 0; k < kPerPhase; k++) acc += ph_[p][k] * x[kPerPhase - 1 - k];
            out(acc);
        }
    }

private:
    float ph_[kUp][kPerPhase];
    float hist_[2 * kPerPhase];
    int pos_ = 0;
};

} // namespace dect2
