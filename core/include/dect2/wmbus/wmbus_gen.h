// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wireless M-Bus test signal: GFSK bursts of simulated meters (water, heat, electricity, gas, heat cost allocator, one with an encrypted payload)
// on the channels of the S1, T1, C1, R2 (868 MHz plan) or N1 (169 MHz plan) modes. Nothing is transmitted: the samples only exist in memory or a file.
//
// SynthConfig (makeWmbusSynth): snrDb (burst power over noise in 250 kHz), cfoHz; modeOpt[0]: 0 the 868 MHz plan (S1, T1, C1 A and B, R2), 1 the 169 MHz plan (N1).
// The capture is centred on the plan's centre (868.625 MHz or 169.4375 MHz).
#pragma once
#include "dect2/engine/mode_synth.h"
#include "wmbus_proto.h"
#include <cstdint>
#include <memory>

namespace dect2 {

struct WmbusGenConfig {
    double rate = 2e6;
    double centreHz = 868.625e6;     // where the capture is centred
    double snrDb = 30;               // burst power over the noise in 250 kHz; >= 99: no noise
    double cfoHz = 0;                // transmitter frequency error
    double amp = 0.4;
    double gapSec = 0.5;             // between bursts
    int plan = 0;                    // 0: 868 MHz (S1, T1, C1 A / B, R2), 1: 169 MHz (N1)
    int onlyPhy = -1;                // >= 0: only this wmbus::Phy
    bool silent = false;             // noise only (tests)
    bool encryptedMeter = true;      // one meter sends an encrypted payload
    uint32_t seed = 1;
};

class WmbusGenerator {
public:
    explicit WmbusGenerator(const WmbusGenConfig& c);
    ~WmbusGenerator();
    void generate(cf32* out, size_t n);
    // the frame bytes (with CRCs) of simulated meter `meter` (0..6) at transmission count `count`; phy returns its mode
    static std::vector<uint8_t> meterFrame(int meter, uint32_t count, wmbus::Phy& phy, bool encrypted = true);
    static bool meterIsWize(int meter);   // a Wize meter: format B, CI 0x20, no data records
    static int meterCount();
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeWmbusSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
