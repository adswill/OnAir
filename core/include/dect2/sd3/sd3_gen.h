// Test signals of the SatDump tier 3 links (sd3_tel.h). Nothing is transmitted: the samples only exist in memory or in a file.
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/sd3/sd3_tel.h"
#include <cstdint>
#include <memory>

namespace dect2 {

struct Sd3GenConfig {
    int profile = 0, link = 0;
    double rate = 6e6;
    double snrDb = 30;           // Es/N0 in the symbol rate; >= 99: no noise
    double cfoHz = 0;
    double driftHzPerS = 0;      // Doppler rate
    double clockPpm = 0;
    double phaseDeg = 0;
    double amp = 0.4;
    double gapSec = 0.01;        // UHF cubesats: noise between the frames
    bool invert = false;         // Reverse the spectrum (Q negated)
    uint32_t seed = 1;
};

class Sd3Generator {
public:
    explicit Sd3Generator(const Sd3GenConfig& c);
    ~Sd3Generator();
    void generate(cf32* out, size_t n);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeSd3Synth(const SynthConfig& cfg, double sampleRate, int profile);

} // namespace dect2
