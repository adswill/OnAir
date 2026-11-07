// Test signal of the Sferics mode: complex noise plus sferics (damped oscillations, a few tens of microseconds) at random times in flashes of 1 to 3
// strokes, optionally with a regular mains impulse train. Nothing is transmitted.
// SynthConfig (makeSfericsSynth): snrDb is the peak SNR of a strike, cfoHz a carrier offset; modeOpt[0]: 0 storm, 1 mains only, 2 storm and mains, 3 quiet;
// modeOpt[1]: 0 50 Hz mains (100 impulses/s), 1 60 Hz (120 impulses/s); modeVal[0] strikes per second (default 1.5).
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct SfericsScene {
    double seconds = 10;
    double noiseRms = 0.03;                  // per sample, both parts together
    std::vector<double> strikeT;             // start times, seconds
    std::vector<double> strikeSnrDb;         // peak power over the noise, dB (same size as strikeT)
    double mainsRate = 0;                    // impulses per second (100 or 120), 0 off
    double mainsSnrDb = 20;
    double mainsPhase = 0.0037;              // time of the first mains impulse, seconds
    double cfoHz = 0;
    double tauUs = 40;                       // decay time of a sferic
    double oscFrac = 0.15;                   // oscillation frequency as a fraction of the sample rate
    uint32_t seed = 1;
};

std::vector<cf32> sfericsRender(const SfericsScene& s, double rate);

std::unique_ptr<ModeSynth> makeSfericsSynth(const SynthConfig& cfg, double sampleRate);

}  // namespace dect2
