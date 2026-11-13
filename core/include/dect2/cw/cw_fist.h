// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CW test signals made by human "fists": the timing of hand-sent Morse (straight key, bug, paddles by ear) and a baseband renderer with
// noise, fading (QSB), drift, key clicks, QRM and noise crashes. Used by the adaptive-decoder tests and the benchmark; nothing is transmitted.
//
// A fist profile gives the sender's speed (with a slow random walk), the spread of the dots and dashes (a bug keeps the dots steady and lets
// the dashes wander), the dash / dot ratio (3 by the book; hand keying runs 2.5 - 4), the weighting (heavy: marks longer and gaps shorter),
// the gaps (lazy character gaps, stretched word gaps) and the swing of paddle sending.
#pragma once
#include <complex>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace dect2 {
namespace cw {

struct FistProfile {
    const char* name = "machine";
    double wpm = 20;
    double speedWalk = 0;       // per character: standard deviation of the random step of log(speed), pulled back to wpm
    double speedSwing = 0;      // a slow swing of the speed: +- this fraction, one cycle in about 20 s
    double dotJit = 0;          // relative standard deviation of a dot
    double dashJit = 0;         // relative standard deviation of a dash
    double dashRatio = 3;       // mean dash / dot
    double dashRatioSd = 0;     // extra standard deviation of a dash, in dots (a bug: 0.4 - 0.7)
    double weight = 0;          // heavy (+) / light (-): added to every mark, taken from every gap, in dots
    double intraJit = 0;        // relative standard deviation of the gap inside a character
    double intraGap = 1;        // mean gap inside a character, in dots
    double charGap = 3;         // mean gap between characters, in dots
    double charGapSd = 0;       // its standard deviation, in dots (lazy: 0.6 - 1)
    double wordGap = 7;         // mean gap between words, in dots (stretched: 9 - 12)
    double wordGapSd = 0;
    double swing = 0;           // paddle swing: a dot after a dash is this fraction longer
};

// the profiles of the benchmark, steadiest first
const std::vector<FistProfile>& fistProfiles();
const FistProfile* fistProfile(const std::string& name);

// a QSO-like text of about `chars` characters (callsigns, reports, names, Q-codes, abbreviations, prosigns "<AR>" "<SK>" "<BT>" "<KN>").
// freeText: random letter and figure groups instead (nothing a vocabulary can guess)
std::string fistText(std::mt19937& rng, int chars, bool freeText = false);

// key-down / key-up durations in seconds (first is a key-down, ends with a key-up) of a text sent by a fist
std::vector<double> fistKeying(const std::string& text, const FistProfile& p, std::mt19937& rng, double wpmOverride = 0);

struct FistRender {
    double rate = 1000;          // samples per second (complex)
    double snrDb = 15;           // key-down carrier to noise in 500 Hz
    double edgeMs = 5;           // raised-cosine key edges
    double clickDb = -200;       // key clicks: an impulse at every key change this far below the carrier (-200: none)
    double qsbDepthDb = 0;       // fading: level swings this far down ...
    double qsbPeriodSec = 0;     // ... in this period (0: none)
    double qsbRandom = 0;        // 0..1: part of the fading that is random (Rayleigh-like, a few seconds) instead of a sine
    double offsetHz = 0;         // carrier frequency in the baseband
    double driftHzPerMin = 0;
    double qrmDb = -200;         // an interfering CW signal this far below the carrier (-200: none) ...
    double qrmOffsetHz = 120;    // ... this far from it
    double crashPerMin = 0;      // noise crashes (impulses, 30 dB over the noise, 2 ms)
    double leadSec = 1.0, tailSec = 3.0;
    uint32_t seed = 1;
};

// the baseband of a keying (durations from fistKeying); an interferer, when asked, sends `qrmDur`
std::vector<std::complex<float>> fistRender(const std::vector<double>& dur, const FistRender& r, const std::vector<double>& qrmDur = {});

} // namespace cw
} // namespace dect2
