// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DECT test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// Base stations each send one beacon burst per 10 ms frame in their own slot: S-field (preamble and RFP sync word), A-field (Nt with the
// RFPI on even frames, Qt system information on odd ones, CRC correct), a random B-field; optionally a handset burst twelve slots later.
// 1.152 Mbit/s GFSK (BT 0.5, 288 kHz deviation), complex white noise, a carrier offset from the config.
//
// makeDectSynth (SynthConfig): snrDb = carrier to noise in 1.152 MHz; cfoHz = carrier offset; modeOpt[0] = 0 two base stations with a
// handset, 1 one base station, 2 noise only.
#pragma once
#include "dect_proto.h"
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct DectGenBase {
    uint64_t rfpi = 0x0100A5C3D1ULL;
    int slot = 0;                     // 0 .. 23
    double ampScale = 1;
    bool inverted = false;            // the complemented form of the A-field CRC
    bool badCrc = false;              // every beacon gets a wrong CRC
};

struct DectGenConfig {
    double rate = 5e6;
    double snrDb = 30;
    double cfoHz = 0;
    double dialOffsetHz = 0;          // where the carrier sits in the capture
    double amp = 0.5;
    uint32_t seed = 1;
    bool noiseOnly = false;
    bool handset = false;             // a handset burst 12 slots after each base station
    std::vector<DectGenBase> bases;
};

class DectGenerator {
public:
    explicit DectGenerator(const DectGenConfig& c);
    ~DectGenerator();
    void generate(cf32* out, size_t n);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::vector<DectGenBase> dectTestBases(int option);
std::unique_ptr<ModeSynth> makeDectSynth(const SynthConfig& cfg, double sampleRate);

}  // namespace dect2
