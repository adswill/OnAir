// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF digital test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// makeHfdigSynth(): one upper sideband transmission on the user's (dial) frequency, at -tuneOffsetHz from 0 Hz in the samples, over
// low-level complex noise (about -31 dBFS). The transmission is the test audio of one decoder (8000 Hz, -1 .. 1), turned into its
// upper sideband (200 .. 3800 Hz above the dial frequency) and brought to the sample rate. Each decoder makes its own test audio in its
// own file through the factories below; while a factory returns nullptr the generator sends the noise alone.
//
// SynthConfig:
//   snrDb       signal to noise ratio in 3 kHz, for test audio with an rms of 0.35 (a sine of amplitude 0.5); louder audio gives more
//   cfoHz       carrier offset (the audio moves up by it)
//   modeOpt[0]  which test signal: 0 RTTY, 1 SSTV, 2 FreeDV, 3 FT8, 4 FT4, 5 WSPR, 6 FT2, 7 HamDRM, 21 JT65, 22 JT9, 23 Q65, 24 FST4, 25 MSK144, 32 JT4
//   modeOpt[1..7], modeVal[0..3]: free for the chosen decoder's test audio (only one plays at a time)
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstddef>
#include <functional>
#include <memory>

namespace dect2 {

std::unique_ptr<ModeSynth> makeHfdigSynth(const SynthConfig& cfg, double sampleRate);

// One decoder's test transmission as audio: 8000 Hz, -1 .. 1, endless.
class HfdigTestAudio {
public:
    virtual ~HfdigTestAudio() = default;
    virtual void generate(float* out, size_t n) = 0;
};

// Defined in each decoder's own file (hfdig_rtty.cpp, hfdig_sstv.cpp, hfdig_freedv.cpp); nullptr = no test signal yet.
std::unique_ptr<HfdigTestAudio> makeRttyTestAudio(const SynthConfig& cfg);
std::unique_ptr<HfdigTestAudio> makeSstvTestAudio(const SynthConfig& cfg);
// One picture as SSTV audio. rgb empty or the wrong size: the test picture. modeIdx as sstvModeInfo().
std::unique_ptr<HfdigTestAudio> makeSstvTx(int modeIdx, const std::vector<uint8_t>& rgb);
std::unique_ptr<HfdigTestAudio> makeFreedvTestAudio(const SynthConfig& cfg);
// Live 8 kHz speech into freedv_tx. pull8k fills n mono samples in -1 .. 1; empty pull keeps the synthetic voice. nullptr without libcodec2.
std::unique_ptr<HfdigTestAudio> makeFreedvTxAudio(const SynthConfig& cfg, std::function<void(float* mono, int n)> pull8k);
std::unique_ptr<HfdigTestAudio> makeFtxTestAudio(const SynthConfig& cfg);
std::unique_ptr<HfdigTestAudio> makeHamdrmTestAudio(const SynthConfig& cfg);   // hfdig_hamdrm_gen.cpp: modeOpt[0] 7   // hfdig_ftx_gen.cpp: modeOpt[0] 3 FT8, 4 FT4, 5 WSPR, 6 FT2
std::unique_ptr<HfdigTestAudio> makeKbdTestAudio(const SynthConfig& cfg);       // hfdig_kbd.cpp: modeOpt[0] 26 .. 31 (hfdig_kbd.h)
// The one modeOpt[0] picks (0 RTTY, 1 SSTV, 2 FreeDV, 3 FT8, 4 FT4, 5 WSPR, 6 FT2, 7 HamDRM, 21 .. 25 JT65, JT9, Q65, FST4, MSK144, 26 .. 31 the keyboard modes; anything else: nullptr)
std::unique_ptr<HfdigTestAudio> makeHfdigTestAudio(int which, const SynthConfig& cfg);

// The same USB (or LSB) mixer as makeHfdigSynth, with audio the caller built. nullptr audio: noise only.
// maxAmp > 0: the upper-sideband amplitude is at most this (multi-carrier audio such as HamDRM keeps its peaks below the transmit limiter's knee)
std::unique_ptr<ModeSynth> makeHfdigSynthFromAudio(std::unique_ptr<HfdigTestAudio> audio, const SynthConfig& cfg, double sampleRate, bool lsb = false, float maxAmp = 0);

} // namespace dect2
