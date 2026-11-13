// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Short adaptive equaliser for the 4-level soft symbols of the C4FM / 4FSK voice links (DMR, P25, NXDN, dPMR, D-STAR is 2-level and keeps its slicer).
//
// Simulcast (P25 LSM) and urban repeaters give an echo of a fraction of a symbol or more: the discriminator output then carries inter-symbol
// interference and the eye closes. The equaliser is a symbol-spaced linear filter over the calibrated soft symbols (levels -3 -1 +1 +3), fitted by
// least squares on a known sync word (ridge-regularised towards "no filter" so a clean channel stays untouched), then refined decision-directed
// over the burst. It is applied only when the eye is measurably closed and kept only when it opens the eye (voicefec / eye metric), so the
// existing slicer stays the default. Off by default (measured: clear gain on symbol-level echo, none yet through the real FM front end); Settings > Advanced
// turns it on (symeq::setEnabled).
#pragma once
#include <cstdint>

namespace dect2::symeq {

bool enabled();
void setEnabled(bool on);

constexpr int kTaps = 11;       // 5 symbols before, the symbol, 5 after
constexpr float kEyeOn = 0.10f; // eye error above which the equaliser is tried
constexpr float kEyeGain = 0.75f; // the equalised eye must be this much better to be used
constexpr float kEyeFrozen = 0.f;   // the same for a burst without a sync word (frozen taps)
constexpr float kSyncGain = 0.7f;  // the error on the known sync symbols must fall to this share
constexpr float kSyncOn = 0.15f;   // mean squared error of the raw sync symbols below which an open eye is left alone

// mean squared distance of the soft symbols to the nearest level (-3 -1 1 3): 0 is a perfect eye, about 1 a closed one
float eyeError(const float* z, int n);

class SymEq {
public:
    SymEq() { reset(); }
    void reset();
    bool active() const { return active_; }
    void setActive(bool a) { active_ = a; }
    const float* taps() const { return w_; }
    // fit the taps on known symbols: z[at + k] should be ref[k] for k < m (levels -3 -1 1 3). ridge pulls towards the identity filter.
    bool train(const float* z, int n, int at, const float* ref, int m, float ridge = 2.f);
    void apply(const float* z, int n, float* y) const;
    // the whole decision for a burst with a known sync at [at, at + m): trains (when ref is given), applies, refines and keeps the result only
    // when it opens the eye. y receives the symbols to slice (a copy of z when the equaliser is not used). Returns true if y is equalised.
    bool run(const float* z, int n, int at, const float* ref, int m, float* y, float* eyeBefore = nullptr, float* eyeAfter = nullptr);
private:
    float w_[kTaps];
    bool active_ = false;
};

} // namespace dect2::symeq
