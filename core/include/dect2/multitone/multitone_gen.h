// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Multitone paging test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// An FM carrier with noise. Multitone's digital format is not public, so the data here is NOT Multitone's: it is FSK with 32-bit words that
// carry the POCSAG BCH(31,21) check, behind an invented 32-bit header, only to exercise the detector. The tone sequence is invented too.
// A 10 s cycle: 1 s noise, a tone sequence (238.3 Hz 0.5 s, 474.6 Hz 0.5 s, 1000 Hz 0.4 s, pauses between), 1 s noise, an FSK burst, noise.
//
// SynthConfig (makeMultitoneSynth): snrDb (carrier over noise in 20 kHz); modeOpt[0]: 0 the cycle, 1 noise only, 2 tones only, 3 data only,
// 4 a standard POCSAG batch (for 4: sync word 0x7CD215D8), 5 audio FFSK (tones 292.7 / 585.3 Hz, 585 baud, as measured on the real recordings): steady high tone, the word 0x605A twice, then the words 0x7B91 0x310E 0xC47F 0xFFFF; was 4 a standard POCSAG batch (sync word 0x7CD215D8: the detector must say so); modeOpt[1]: baud of the data, 0 = 1200, 1 = 512, 2 = 2400.
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct MultitoneGenConfig {
    double rate = 2e6;
    double snrDb = 30;
    int content = 0;            // as modeOpt[0]
    double baud = 1200;
    double devData = 4000, devTone = 2500;   // Hz
    double carrierOffsetHz = 0; // the carrier's error from the channel centre
    bool invert = false;        // data polarity
    uint32_t seed = 1;
    int words = 12;             // code words of the data burst
};

class MultitoneGenerator {
public:
    explicit MultitoneGenerator(const MultitoneGenConfig& c);
    ~MultitoneGenerator();
    void generate(cf32* out, size_t n);
    double cycleSec() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The 32 test words of the data burst (invented header, then BCH-protected words)
std::vector<uint32_t> multitoneTestWords(int count);
std::vector<uint16_t> multitoneFfskWords();   // what content 5 sends after the steady tone

std::unique_ptr<ModeSynth> makeMultitoneSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
