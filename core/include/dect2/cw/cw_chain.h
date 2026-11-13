// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CW signal chain after the channel filter: the magnitude of the channel once a millisecond in, decoded text out.
//
// Envelope detector -> debounce -> decoder. Two decoders: the adaptive one (cw_decoder.h: learns the operator, reads hand keying, character
// confidence; the default) and the fixed-clustering one of 0.2.7 (cw_morse.h Timing), kept for comparison and as a fallback.
//
// The detector smooths the magnitude over a fraction of the dot length (a matched filter whose bandwidth follows the speed), keeps the mark
// level (fast attack, a decay that follows a fade) and the noise level (updated only in the gaps) apart, switches on a Schmitt trigger
// between them, clips impulses (noise crashes, key clicks of a neighbour) and stays shut until the mark level stands clearly over the noise.
#pragma once
#include "dect2/cw/cw_decoder.h"
#include "dect2/cw/cw_morse.h"
#include <memory>
#include <string>

namespace dect2 {
namespace cw {

struct ChainTune {
    float smoothFrac = 0.15f;    // envelope smoothing length, in dots
    float smoothWeak = 0.0f;    // added when the mark level is under weakRatio times the noise
    float weakRatio = 4.0f;
    float smoothMin = 2.0f, smoothMax = 20.0f;   // ms
    float thrOn = 0.55f, thrOff = 0.42f;         // Schmitt thresholds, fractions of (mark - noise)
    float attackIdle = 0.006f;   // how fast the mark level rises while no mark is on (noise peaks must not open the gate)
    float hiDecayDots = 10.f;    // ... at least this many dots
    float hiDecayMs = 700.f;     // the mark level falls toward the noise in the gaps with this time constant
    float openRatio = 2.6f;      // mark level over noise level needed to decode
    float debounceFrac = 0.20f;  // a change counts after this fraction of a dot (min 3, max 15 ms)
};
ChainTune& chainTune();

class Chain {
public:
    Chain();
    ~Chain();
    void reset();
    void setAdaptive(bool on);          // default true
    bool adaptive() const;
    void setManualWpm(double wpm);      // 0: automatic
    void sample(float magnitude);       // one millisecond
    bool key() const;
    bool locked() const;
    double wpm() const;
    bool farnsworth() const;
    float markLevel() const;
    float noiseLevel() const;
    // appends the text decoded since the last call; conf: one digit '0'-'9' per character (9 = sure), alt: the best other reading per
    // character (' ' none; a prosign shows as '^')
    void take(std::string& text, std::string& conf, std::string& alt);
    void stats(FistStats& s) const;     // the learned operator (adaptive only; legacy fills the speed)
    int liveAlternatives(char (*out)[48], int maxN) const;
    uint64_t unknownChars() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace cw
} // namespace dect2
