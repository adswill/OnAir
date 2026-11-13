// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NOAA Weather Radio SAME test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// One NFM weather channel: AFSK at 520.83 baud (mark 2083.3 Hz, space 1562.5 Hz), each message as 16 x 0xAB then the text, three times one
// second apart, an optional 1050 Hz attention tone, and "NNNN" three times. Complex white noise and a carrier offset come from the config.
//
// SameGenerator: any messages in any order (tests; corrupted repeats, dropped repeats, back to back alerts).
// makeSameSynth (SynthConfig): snrDb = carrier to noise in 25 kHz; cfoHz = carrier offset; modeOpt[0] = 0 two alerts back to back (a tornado
// warning for three locations with partial county codes and a required weekly test, with tone and ends; about 19 s), 1 noise only,
// 2 the tornado warning alone (about 10 s).
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct SameGenMessage {
    std::string text;               // the header text "ZCZC-...-" (or any text: sent as it is)
    int repeats = 3;
    int corruptBurst = -1;          // burst (0 .. repeats - 1) that gets bit errors in the text part
    int corruptBits = 0;            // how many bits are flipped
    int dropBurst = -1;             // burst left out
    double toneSec = 0;             // 1050 Hz tone after the header (0 none)
    bool sendEom = true;            // "NNNN" three times after the tone
    double leadSec = 0.6;           // quiet before it
    double gapSec = 1.0;            // between repeats
};

struct SameGenConfig {
    double rate = 2e6;
    double snrDb = 25;              // carrier to noise in 25 kHz
    double cfoHz = 0;               // carrier offset from the channel
    double tuneOffsetHz = 50000;    // the radio sits this far above the channel (sameTuning().tuneOffsetHz)
    uint32_t seed = 1;
    bool noiseOnly = false;
    bool loop = true;               // the messages again from the start when they end
    std::vector<SameGenMessage> messages;
};

class SameGenerator {
public:
    explicit SameGenerator(const SameGenConfig& c);
    ~SameGenerator();
    void generate(cf32* out, size_t n);
    double cycleSec() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The test messages of the synthetic source and the tests
std::string sameTestTornado();      // ZCZC-WXR-TOR-048201-348113-029047+0030-1301530-KHGX/NWS-
std::string sameTestWeekly();       // ZCZC-WXR-RWT-029047+0015-1301545-KEAX/NWS-

std::unique_ptr<ModeSynth> makeSameSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
