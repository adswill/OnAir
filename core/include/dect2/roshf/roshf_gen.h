// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ROS (HF) test signal. Nothing is transmitted: the samples only exist in memory or in a file.
// The publicly reported physical layer only (ros/ros_detect.h): one phase-continuous tone at a time out of 144 on a 15.625 Hz grid, chosen at
// random every symbol (4, 8 or 16 baud), in the upper sideband of a radio, in white noise, with a tuning offset and a drift. It is NOT ROS
// encoding (there is no public ROS framing, coding or FEC). Also a noise-only mode and a few signals that are not ROS (a wide FSK, a single tone,
// a 2-tone RTTY) for the negative tests.
// SynthConfig (makeRoshfSynth): snrDb is the SNR in 2.5 kHz, cfoHz the radio's tuning error, modeVal[0] the baud rate (default 16), modeOpt[0]
// 0 ROS, 1 noise only, 2 RTTY-like 2-tone FSK, 3 a single carrier.
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstddef>
#include <cstdint>
#include <memory>

namespace dect2 {

struct RoshfGenConfig {
    double rate = 2e6;
    double dialOffsetHz = 0;       // where the dial frequency sits in the samples
    double audioCentreHz = 1550;   // the middle of the tone grid in the sideband audio
    double baud = 16;
    int tones = 144;
    double spacingHz = 15.625;
    int kind = 0;                  // 0 ROS-like, 1 noise only, 2 RTTY-like (two tones 170 Hz apart, 45.45 baud), 3 a single carrier
    double cfoHz = 0;
    double driftHzPerSec = 0;
    double snrDb = 10;             // tone SNR in 2.5 kHz; >= 99: no noise
    double clockPpm = 0;
    double amp = 0.3;
    bool lsb = false;
    uint32_t seed = 1;
};

class RoshfGenerator {
public:
    explicit RoshfGenerator(const RoshfGenConfig& c);
    ~RoshfGenerator();
    void generate(cf32* out, size_t n);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeRoshfSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
