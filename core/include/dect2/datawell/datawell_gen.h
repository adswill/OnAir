// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Datawell buoy HF link test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// 2FSK (format 0, the HXV format): continuous-phase 2FSK, tones 129 Hz apart, 81.92 baud, carrying 64-bit vectors built from the documented
// HXV layout (datawell_hxv.h): sync word, system file words, spectrum words, sinusoidal heave / north / west displacements, BCH parity, WR-SG option.
// 4FSK (format 1): four tones 78 Hz apart at 81.92 baud carrying a SYNTHETIC test frame of our own (0xD391, 12 random bytes, CRC-16/CCITT-FALSE,
// 128 bits): the over-the-air format of the HVA buoys is not public, so only the modem and the frame finder can be tested with it.
//
// SynthConfig (makeDatawellSynth): snrDb (in 100 Hz); modeOpt[0]: 0 2FSK, 1 4FSK, 2 noise only, 3 a plain carrier; modeOpt[1]: 1 inverted
// polarity; modeOpt[2]: 1 WR-SG vectors; modeVal[0]: offset of the signal from the radio's centre, Hz (0: 1500); modeVal[1]: symbol clock error, ppm.
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

// The test sea state of the 2FSK signal, in raw words of the documented layout (so a test can check every decoded number)
struct DwTestSpecRaw { int n = 0; unsigned dir = 0, slsb = 0, m2lsb = 0, n2lsb = 0, rpsd = 0, s = 0, m2 = 0, n2 = 0, k = 0; };
unsigned dwTestSysWord(int num);                 // the 12 bit system file word number num
DwTestSpecRaw dwTestSpecRaw(int n);
uint16_t dwTestSpecWord(int n, int k);           // word k (0 .. 3) of the spectrum entry n
void dwTestDisp(double t, int& h, int& n, int& w);   // cm

struct DatawellGenConfig {
    double rate = 250e3;
    double snrDb = 20;             // in 100 Hz
    int format = 0;                // 0 2FSK, 1 4MFSK
    bool noiseOnly = false;
    bool carrierOnly = false;
    bool invert = false;
    bool wrsg = false;             // 2FSK: the WR-SG vector (two vertical samples and the 0xAAA dummy)
    double offsetHz = 1500;
    double clockPpm = 0;
    uint32_t seed = 1;
};

class DatawellGenerator {
public:
    explicit DatawellGenerator(const DatawellGenConfig& c);
    ~DatawellGenerator();
    void generate(cf32* out, size_t n);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

constexpr int kDwTestFrameBits = 128;
constexpr uint16_t kDwTestSync = 0xD391;

std::unique_ptr<ModeSynth> makeDatawellSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
