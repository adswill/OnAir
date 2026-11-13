// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FM audio tone control: a bass shelf and a treble shelf (each -6 .. +6 dB) and the Warm preset, a gentle -3 dB high shelf above 6 kHz.
// It works on the 48 kHz stereo audio of the FM receiver. All zero and Warm off is the identity: the audio passes untouched.
#pragma once
#include <cstddef>

namespace dect2 {

// a normalised biquad, y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2
struct FmShelf { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

constexpr double kToneBassHz = 200, kToneTrebleHz = 6000, kWarmCutDb = 3, kToneMaxDb = 6;

// RBJ cookbook shelves (slope 1): the low shelf gives gainDb at DC and 0 dB well above fc, the high shelf the reverse
FmShelf fmLowShelf(double gainDb, double fc, double fs);
FmShelf fmHighShelf(double gainDb, double fc, double fs);
// the gain of a shelf at frequency f, in dB
double fmShelfDb(const FmShelf& s, double f, double fs);
// the treble shelf's gain: the control (clamped to +-6 dB) with the Warm cut taken off
double fmTrebleGainDb(int trebleDb, bool warm);

class FmTone {
public:
    // bass and treble in dB (clamped to -6 .. +6); warm adds the -3 dB cut above 6 kHz; fs is the audio rate
    void configure(int bassDb, int trebleDb, bool warm, double fs);
    bool active() const { return on_; }
    void reset();
    void process(float* left, float* right, size_t n);   // in place
private:
    bool on_ = false;
    FmShelf bass_, treble_;
    double sb_[2][2] = {}, st_[2][2] = {};   // per channel, the two state values of each shelf
};

} // namespace dect2
