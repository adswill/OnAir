// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Audio equaliser (docs/AUDIO-EQ.md): bass and treble shelves plus a 10-band graphic equaliser (octave bands 31 Hz .. 16 kHz, +-12 dB), with
// presets (voice, music, hearing-friendly ...). A stage of the audio cleanup chain (audio_cleanup.h); also usable on its own. Biquads in
// double precision, own code from the published RBJ cookbook formulas. process() never allocates.
#pragma once
#include <array>
#include <cstddef>

namespace dect2 {

constexpr int kToneBands = 10;
constexpr float kToneMaxDb = 12.f;
extern const float kToneBandHz[kToneBands];   // 31.5, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000

struct ToneParams {
    float bass = 0, treble = 0;                 // dB, low shelf at 120 Hz, high shelf at 6 kHz
    std::array<float, kToneBands> band{};         // dB per band
    bool flat() const;                          // every gain under 0.05 dB: the equaliser does nothing
    void clamp();                               // every gain into +-12 dB (NaN becomes 0)
    bool operator==(const ToneParams& o) const { return bass == o.bass && treble == o.treble && band == o.band; }
};

enum class ToneEqPreset { Flat = 0, Voice, Music, Hearing, BassBoost, Bright, Count };
const char* toneEqPresetName(ToneEqPreset p);           // "Flat", "Voice", "Music", "Hearing-friendly", "Bass boost", "Bright"
ToneParams toneEqPresetParams(ToneEqPreset p);
// The preset the parameters equal (to within 0.5 dB), or Count when they are the user's own curve.
ToneEqPreset toneEqPresetOf(const ToneParams& p);

class ToneEq {
public:
    void configure(const ToneParams& p, float rate);   // recomputes only when something changed; bands at or above 0.45 x rate are left out
    float tick(float x);                             // one sample
    void process(float* x, int n) { for (int i = 0; i < n; i++) x[i] = tick(x[i]); }
    void reset();
    // The gain in dB the current setting gives at `hz` (for drawing the curve and for tests).
    double responseDb(double hz) const;
private:
    struct Bq { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0; };
    static constexpr int kN = kToneBands + 2;
    Bq q_[kN];
    bool used_[kN] = {};
    ToneParams cur_;
    float rate_ = 0;
    bool have_ = false;
};

}  // namespace dect2
