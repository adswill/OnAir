// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Analog FPV video test signal: a video transmitter on one channel of the plans (fpv_chan.h), as it is on the air: the composite video of the analog
// TV generator (test card or colour bars, PAL or NTSC, with the clock and a moving box) frequency modulated onto the carrier (sync tip the lowest
// frequency, a swing of +-8 MHz), with a sound subcarrier of 6.0 or 6.5 MHz (FM, a 1 kHz tone with short interruptions) added to the composite.
// Nothing is transmitted: the samples only exist in memory or a file.
//
// SynthConfig (makeFpvSynth): snrDb (carrier to noise in 17 MHz; 100 and above: no noise), cfoHz (the transmitter's frequency error);
//   modeOpt[0] system 0 PAL 625 lines, 1 NTSC 525 lines, modeOpt[1] picture 0 test card, 1 colour bars,
//   modeOpt[2] sound 0 subcarrier 6.5 MHz, 1 subcarrier 6.0 MHz, 2 none, modeOpt[3] 1 = no transmitter (noise only),
//   modeVal[0] where the capture is centred, in MHz from the transmitter (default 0: the transmitter is on the centre; the receiver takes the
//   channel of the plans nearest to the radio's centre, so the test signal is a transmitter on whatever channel the radio is tuned to).
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>

namespace dect2 {

struct FpvGenConfig {
    double rate = 20e6;
    double cnrDb = 30;           // carrier to noise in 17 MHz; 100 and above: no noise
    double cfoHz = 0;            // the transmitter's frequency error
    double offsetHz = 0;         // the transmitter's channel above the centre of the capture
    double devHz = 8e6;          // sync tip to white swing is +-devHz
    int ntsc = 0;                // 0 PAL 625 lines, 1 NTSC 525 lines
    int pattern = 0;             // 0 test card, 1 colour bars
    int sound = 1;               // 0 none, 1 subcarrier 6.5 MHz, 2 subcarrier 6.0 MHz
    double soundDevHz = 25e3;
    double soundLevel = 0.12;    // subcarrier amplitude against the video swing of 1
    bool syncLow = true;         // the sync tip is the lowest frequency
    double acTauMs = 0;          // > 0: the transmitter is AC coupled (a high-pass in front of the modulator)
    bool off = false;            // no transmitter: noise only
    double amp = 0.3;
    uint64_t seed = 1;
};

class FpvGenerator {
public:
    explicit FpvGenerator(const FpvGenConfig& c);
    ~FpvGenerator();
    void generate(cf32* out, size_t n);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeFpvSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
