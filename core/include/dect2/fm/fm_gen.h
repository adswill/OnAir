// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FM broadcast transmitter for tests: stereo multiplex with pilot, RDS (station name, radio text) and noise, as complex baseband.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/fm/rds_live.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <array>
#include <string>
#include <vector>

namespace dect2 {

struct FmGenConfig {
    double rate = 2e6;            // output sample rate, Hz
    double cnrDb = 40;            // carrier to noise in 200 kHz; 200 = no noise
    double cfoHz = 0;             // carrier offset
    bool stereo = true;           // send the pilot and the difference signal
    double leftHz = 1000, rightHz = 3000;
    float leftAmp = 0.5f, rightAmp = 0.5f;   // 0 = silent
    double pilotPct = 9;          // pilot injection, % of 75 kHz
    bool rds = true;
    double rdsDevKhz = 2;         // RDS injection
    std::string ps = "TESTFM  ";  // 8 characters
    std::string rt = "OnAir FM test signal";
    int pi = 0x4A21;
    int pty = 10;
    bool tp = true, ta = false, music = true;
    // extra RDS groups sent round the station groups (blocks B, C, D; B complete with type, TP and PTY); a test can send TMC (3A, 8A) with them
    std::vector<std::array<uint16_t, 3>> extraGroups;
    // Live RDS: when set, the encoder takes the station name, text, PI, PTY and flags from it at the start of each cycle of groups (a change needs no restart)
    std::shared_ptr<RdsLive> live;
    double preemphUs = 50;
    double deviationHz = 75000;   // 75 kHz broadcast; NFM uses a separate generator
    uint32_t seed = 1;
    // Live L/R at 48 kHz stereo (the transmitter's microphone or a file). Empty: the leftHz / rightHz tones.
    std::function<void(float* stereo, int frames)> pull48k;
};

class FmGenerator {
public:
    explicit FmGenerator(const FmGenConfig& c);
    void generate(size_t n, std::vector<cf32>& out);   // appends n samples
    void generate(cf32* out, size_t n, float gain = 1.f);   // writes n samples, scaled by gain

private:
    void nextGroup();
    size_t extraNo_ = 0;
    RdsState cur_;                 // what the groups carry now (a copy of the live state, refreshed at the start of a cycle)
    bool curInit_ = false;
    int rtSeg_ = 0;                // next radio text segment
    int rtAb_ = 0;
    std::string rtCur_;
    int ctGroups_ = 0;
    FmGenConfig c_;
    std::mt19937 rng_;
    double t_ = 0;                 // seconds
    double ph_ = 0;                // carrier phase
    double lph_ = 0, rph_ = 0;
    // oscillators by recurrence (real, imaginary): the two tones, the pilot, the RDS bit clock; renormalised every block
    double lo_[2] = {1, 0}, ro_[2] = {1, 0}, po_[2] = {1, 0}, bo_[2] = {1, 0};
    uint64_t noiseState_ = 0;      // pseudo random positions in the Gaussian table
    double lGain_ = 1, rGain_ = 1; // pre-emphasis gain at the tone frequencies
    std::vector<uint8_t> bits_;    // differentially coded RDS bits, queued
    size_t bitBase_ = 0;           // stream index of bits_[0]
    uint8_t lastDiff_ = 0;
    int groupNo_ = 0;
    std::vector<float> live_;     // interleaved stereo at 48 kHz, pre-emphasised
    size_t livePos_ = 0;
    double liveFrac_ = 1;
    float liveL_[2] = {0, 0}, liveR_[2] = {0, 0};
    float preL_ = 0, preR_ = 0;
    void pullLive();
};

} // namespace dect2
