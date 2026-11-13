// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Test signals of the HF beacon mode: keyed carriers at known frequencies, levels and times in complex Gaussian noise, with slow fading.
// A source is a carrier at freqHz (an RF frequency; the generator is "tuned" to centreHz) whose amplitude follows the keying of its kind:
//  - Letter: the Morse of the marker letter at `unit` seconds per dot, `gapUnits` between repeats
//  - Dashes: 2.9 s dashes, one per 3 s step of the 27 s ladder (step `dashIdx`, the cycle starting at `cycle0`; step 0 also sends one at step 1)
//  - Ncdxf: the callsign in Morse at 22 wpm from 0.1 s into the 10 s slot of beacon `ncdxfIdx` on band `band`, then four 1 s dashes from `t4`
//    (100 W, 10 W, 1 W, 0.1 W: 0, -10, -20, -30 dB) separated by `dashGap` s, in the slots of the 18 beacon cycle on the generator's UTC clock
//  - Carrier: steady
// snr100Db is the SNR in 100 Hz of the key-down carrier (the 100 W dash for an NCDXF beacon) before fading.
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2::hfb {

struct GenSrc {
    enum { Carrier, Letter, Dashes, Ncdxf };
    int kind = Carrier;
    double freqHz = 0, snr100Db = 20;
    char letter = 'D'; double unit = 0.08, gapUnits = 7;
    int dashIdx = 0; double cycle0 = 0;
    int ncdxfIdx = 0, band = 0; double t4 = 4.4, dashGap = 0.0;
    double fadeDepthDb = 0, fadePeriodSec = 20;
    double from = -1e30, to = 1e30;    // active window, UTC seconds
};

class Gen {
public:
    Gen(double rate, double centreHz, double startUtc, double noiseVar, uint64_t seed);
    void add(const GenSrc& s);
    void generate(cf32* out, size_t n);
    double utc() const { return t_; }
    void setNoise(bool on) { noise_ = on; }   // false: the beacons only (what a transmitter sends); the levels stay as if the noise were there
private:
    bool noise_ = true;
    struct Src { GenSrc s; double phase = 0, amp = 0; std::vector<std::pair<bool, double>> pat; double patLen = 0; };
    double envelope(const Src& x, double t) const;
    double rate_, centre_, t_, var_;
    uint64_t rng_;
    std::vector<Src> src_;
    double gauss();
    bool haveSpare_ = false; double spare_ = 0;
};

} // namespace dect2::hfb

namespace dect2 {
constexpr double kHfbSynthDefMhz = 3.65;
// The test signal of the mode: single-letter beacons D and K (keyed at +15 dB in 100 Hz) among the 3.59 - 3.6 MHz channels (centred 3.65 MHz) and a steady carrier
// that is no beacon; SynthConfig::modeOpt[0] = 1: noise only. Centred on SynthConfig::modeVal[0] MHz (0: kHfbSynthDefMhz).
std::unique_ptr<ModeSynth> makeHfbeaconSynth(const SynthConfig& cfg, double sampleRate);
}
