// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AM audio processor of the general receiver: everything DSP can do to make an AM broadcast clear, with one "Auto quality" switch.
//
//   complex 48 kHz baseband (the carrier near 0 Hz, the band +-12 kHz or wider)
//   -> noise blanker (impulses on the IF magnitude, held over)
//   -> carrier: found by an FFT within +-3 kHz, then a PLL (fast pull-in, narrow when locked, holds through fades)
//   -> synchronous detection of both sidebands, or of the cleaner one (ECSS) when the other carries interference; envelope as fallback
//   -> adaptive bandwidth: the audio edge where the received spectrum falls into the noise or interference starts (2.5 to 10 kHz)
//   -> notch bank: interfering carriers (whistles: a line on one sideband only) and mains hum (50/60 Hz series)
//   -> noise reduction: MMSE log-spectral amplitude (Ephraim-Malah) with a minimum-statistics noise tracker, gain floor against musical noise
//   -> leveler (AGC with hang) and a look-ahead peak limiter, DC removal
//   -> optional: a gentle presence lift when the bandwidth is narrow, and an "enhanced" bandwidth extension above the received edge
//
// Physics: AM has no noise immunity like FM. The processor removes what is not the programme (noise, clicks, whistles, the other station's
// sideband) and can lift what is there; it cannot bring back audio that was never sent or that the noise covers. The extension above the
// received edge is synthetic (made from the audio below it) and labelled "enhanced"; it is off by default.
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct AmProcConfig {
    bool autoQuality = true;     // pick detector, sideband, bandwidth, blanker, noise reduction, notches and EQ from what is measured
    int detector = -1;           // -1 auto, 0 envelope, 1 synchronous both sidebands, 2 upper only, 3 lower only
    int bandwidthHz = 0;         // audio bandwidth (the channel is twice this): 0 auto, else 2500 .. 10000
    int nr = -1;                 // noise reduction: -1 auto, 0 off, 1 .. 100 strength
    int nbDb = -1;               // noise blanker: -1 auto, 0 off, else the threshold in dB above the mean (6 .. 30)
    int notch = -1;              // whistle and hum notches: -1 auto, 0 off
    int eq = -1;                 // presence lift: -1 auto (only on narrow audio), 0 flat
    bool enhance = false;        // synthetic extension above the received audio edge ("enhanced")
    bool agc = true;             // the level follows the carrier, then the limiter; off: the fixed gain below
    bool leveler = true;         // constant loudness (slow AGC on the audio, +-12 dB) before the limiter
    float gainDb = 20;
};

struct AmProcStatus {
    bool carrier = false;        // a carrier was found
    bool locked = false;         // the PLL is locked (synchronous detection possible)
    double carrierHz = 0;        // the carrier's offset from the tuned frequency
    float carrierDb = -200;      // carrier level, dB full scale
    float snrDb = 0;             // audio signal to noise (programme band against the noise floor)
    int detector = 0;            // in use: 0 envelope, 1 synchronous, 2 upper, 3 lower
    bool ecss = false;           // one sideband chosen because the other carries interference
    float upperInterf = 0, lowerInterf = 0;   // interference measure of each sideband (0 none)
    float bandwidthHz = 0;       // audio bandwidth in use
    float nrDb = 0;              // noise reduction gain floor in use (0 off)
    float blankPct = 0;          // share of samples blanked over the last second
    float nbThrDb = 0;           // the blanker's threshold (0 off)
    std::vector<float> notchHz;  // whistles notched
    int humHz = 0;               // 50 or 60 when mains hum is notched
    bool fading = false;         // the carrier is in a fade (the PLL holds)
    bool enhanced = false;
    float levelDb = 0;           // leveler gain
    std::string text;            // plain words: what the processor is doing
};

class AmProcessor {
public:
    AmProcessor();
    ~AmProcessor();
    void configure(double rate);                 // the input and output rate (48000)
    void reset();
    void setConfig(const AmProcConfig& c);
    // n complex samples in, n audio samples appended to out (the processing delay is about 40 ms)
    void process(const cf32* x, size_t n, std::vector<float>& out);
    AmProcStatus status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
