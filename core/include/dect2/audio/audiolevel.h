// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Loudness of the sound output: the volume slider's taper (0 = mute, 1 = 0 dB, one step above zero about -50 dB) and a leveller that only
// turns loud audio down towards a common nominal level (it never boosts, so the noise between transmissions stays quiet), plus a soft limiter.
#pragma once
#include <cmath>

namespace dect2 {

constexpr float kVolumeFloorDb = -50.f;     // slider position just above 0
constexpr float kNominalRmsDb = -23.f;      // the level every mode is leveled down to when louder

// slider position 0..1 -> linear gain: 0 = silence, 1 = 0 dB, in between a straight line in dB
inline float volumeTaper(float pos) {
    if (!(pos > 0.f)) return 0.f;
    if (pos >= 1.f) return 1.f;
    return std::pow(10.f, kVolumeFloorDb * (1.f - pos) / 20.f);
}

inline float softLimit(float x) {
    const float a = std::fabs(x), k = 0.8f;
    if (a <= k) return x;
    const float y = k + (1.f - k) * std::tanh((a - k) / (1.f - k));
    return x < 0 ? -y : y;
}

class AudioLeveler {
public:
    // interleaved stereo, in place
    void process(float* s, int frames) {
        const float target = std::pow(10.f, kNominalRmsDb / 20.f);
        for (int i = 0; i < frames; i++) {
            const float l = s[2 * i], r = s[2 * i + 1];
            const float p = 0.5f * (l * l + r * r);
            pw_ += (p - pw_) * kPow;   // a symmetric window: an unequal one reads a steady tone too high
            const float rms = std::sqrt(pw_);
            const float want = rms > target ? target / rms : 1.f;
            gain_ += (want - gain_) * (want < gain_ ? kGainAtk : kGainRel);
            s[2 * i] = softLimit(l * gain_);
            s[2 * i + 1] = softLimit(r * gain_);
        }
    }
    void reset() { pw_ = 0.f; gain_ = 1.f; }
private:
    // time constants at 48 kHz: power 50 ms, gain 5 ms down / 500 ms up
    static constexpr float kPow = 1.f / 2400.f, kGainAtk = 1.f / 240.f, kGainRel = 1.f / 24000.f;
    float pw_ = 0.f, gain_ = 1.f;
};

} // namespace dect2
