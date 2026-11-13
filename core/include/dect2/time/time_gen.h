// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Time signal test signal: DCF77, MSF, WWVB, JJY, WWV / WWVH or CHU carrying a known time. Nothing is transmitted: the samples only exist in
// memory or in a file.
//
// LF stations: the carrier keyed as the station keys it (time_proto.h), at dialOffsetHz in the samples. HF stations: an AM carrier with the
// station's audio: WWV / WWVH ticks (1000 / 1200 Hz, 5 ms), the minute tone and the 100 Hz BCD subcarrier; CHU ticks and the 300 bit/s
// Bell 103 FSK frames (B in second 31, A in 32 - 39).
//
// SynthConfig (makeTimeSynth): modeOpt[0] station + 1 (0: DCF77), modeOpt[1] 1: WWVH instead of WWV; snrDb (carrier to noise in 1 kHz), cfoHz.
// The test signal starts at the computer's clock, so the app shows the computer clock as right.
#pragma once
#include "dect2/engine/mode_synth.h"
#include "time_proto.h"
#include <cstdint>
#include <memory>

namespace dect2 {

struct TimeGenConfig {
    int station = timesig::Dcf77;
    bool wwvh = false;            // WWV / WWVH: the WWVH ticks (1200 Hz)
    double rate = 256000;
    double dialOffsetHz = 0;      // where the carrier sits in the samples
    double snrDb = 40;            // carrier to noise in 1 kHz; >= 99: no noise
    double cfoHz = 0;
    double amp = 0.3;
    double startUtc = 0;          // UTC epoch seconds of the first sample (0: the computer's clock now)
    double delayMs = 0;           // the path delay: the seconds arrive this much after UTC
    double clockPpm = 0;          // the receiving radio's sample clock error
    uint32_t seed = 1;
    timesig::EncodeOpts opts;
};

class TimeGenerator {
public:
    explicit TimeGenerator(const TimeGenConfig& c);
    ~TimeGenerator();
    void generate(cf32* out, size_t n);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeTimeSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
