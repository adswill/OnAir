// FM broadcast transmitter for tests: stereo multiplex with pilot, RDS (station name, radio text) and noise, as complex baseband.
#pragma once
#include "ring.h"
#include <cstdint>
#include <random>
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
    double preemphUs = 50;
    uint32_t seed = 1;
};

class FmGenerator {
public:
    explicit FmGenerator(const FmGenConfig& c);
    void generate(size_t n, std::vector<cf32>& out);   // appends n samples
    void generate(cf32* out, size_t n, float gain = 1.f);   // writes n samples, scaled by gain

private:
    void nextGroup();
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
};

} // namespace dect2
