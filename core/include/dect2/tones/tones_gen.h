// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Tone signalling test signals. Nothing is transmitted: the samples only exist in memory or in a file.
//
// ToneScene builds an audio signal (1.0 = 5 kHz FM deviation) out of CTCSS, DCS, DTMF, sequential, SELCAL and two-tone parts, speech-like
// interference and noise; tests feed it to the receiver as audio, or FM-modulate it (toneSceneFm) onto a channel.
// makeTonesSynth (SynthConfig): snrDb = carrier to noise in 25 kHz; cfoHz = carrier offset; modeOpt[0] = 0 a looping demonstration of 40 s
// (CTCSS 123.0 Hz under speech-like noise, a DCS code, DTMF, a ZVEI1 call, SELCAL, a two-tone page), 1 noise only.
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

class ToneScene {
public:
    explicit ToneScene(double rate = 48000, uint32_t seed = 1) : rate_(rate), seed_(seed) {}
    double rate() const { return rate_; }
    const std::vector<float>& audio() const { return a_; }
    double seconds() const { return (double)a_.size() / rate_; }
    void pad(double sec);                                                   // make the scene at least this long
    void tone(double hz, double t0, double sec, double amp);               // a plain tone (added)
    void ctcss(double hz, double t0, double sec, double amp);              // = tone()
    void dcs(int code, bool inverted, double t0, double sec, double amp, double startBit = 0);   // 134.4 bit/s NRZ, low-passed
    void dtmf(const std::string& keys, double t0, double toneSec, double gapSec, double amp);
    // sequential tones: set id of tones_proto.h ("zvei1", "ccir" ...), digits "0".."9"; a repeated digit goes out as the repeat tone
    void sequential(const char* setId, const std::string& digits, double t0, double toneMs, double amp);
    void selcal(const std::string& code, double t0, double amp, double pairSec = 1.0, double gapSec = 0.2);   // "ABCD" letters
    void twoTone(double hzA, double hzB, double t0, double secA, double secB, double amp);
    void speech(double t0, double sec, double amp);                         // voiced, gliding pitch, syllables and fricatives
    void noise(double t0, double sec, double rms);
    void normalize(double peak);                                            // scale so the largest sample is peak

private:
    double rate_;
    uint32_t seed_;
    std::vector<float> a_;
};

// FM-modulate the scene: complex baseband at `rate`, carrier offset by offsetHz, deviation 5 kHz at 1.0, complex noise for snrDb in 25 kHz
std::vector<cf32> toneSceneFm(const ToneScene& s, double rate, double offsetHz, double snrDb, uint32_t seed = 3);

std::unique_ptr<ModeSynth> makeTonesSynth(const SynthConfig& cfg, double sampleRate);
// The SELCAL mode's demonstration: voice, then the calls AM-CP, DH-KS, AM-CP in a loop of 22 s (same SynthConfig as makeTonesSynth)
std::unique_ptr<ModeSynth> makeSelcalSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
