// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LoRa satellite test signal: LoRa beacon frames of a satellite pass over complex white noise. The carrier moves with the satellite's Doppler shift
// (a straight-line pass model: shift = f v / c * (tca - t) / sqrt((t - tca)^2 + (d / v)^2)), tilts through each frame with the Doppler rate, and the
// symbol clock is stretched with it. Nothing is transmitted: the samples only exist in memory or in a file. Uses the LoRa modulator of mesh_lora.h.
//
// SynthConfig (makeLoraSatSynth): snrDb = SNR of the frames in their bandwidth; cfoHz added to every frame (a radio's frequency error); sroPpm = the
// transmitter's own clock error on top of the Doppler stretch.
//   modeOpt[0]  0 a 10 minute pass at 436.8 MHz overhead (beacons every 6 s), 1 a pass at 868.1 MHz
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/mesh/mesh_lora.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct LoraSatPass {
    double carrierMhz = 436.8;     // the nominal frequency
    double speedMS = 7560;         // the satellite's speed
    double closestKm = 600;        // distance at the closest approach (the altitude for a pass overhead)
    double tcaSec = 300;           // time of the closest approach, pass time
    double dopplerHz(double passSec) const;
    double rateHzS(double passSec) const;          // d(shift)/dt
    double stretchPpm(double passSec) const;       // how much the satellite's clock appears slow (+) or fast (-)
};

struct LoraSatGenFrame {
    lora::Params p;
    std::vector<uint8_t> payload;
    double startSec = 0;           // signal time
    double passSec = 0;            // pass time the Doppler is taken at
    double snrOffDb = 0;
};

struct LoraSatGenConfig {
    double rate = 2e6;
    double dialOffsetHz = 0;       // where the dial sits in the samples
    double snrDb = 10;             // SNR of a frame in its bandwidth; >= 99: no noise
    double cfoHz = 0;
    double sroPpm = 0;
    double amp = 0.3;
    double leadSec = 0.05;
    bool dopplerOn = true;         // off: the carrier stays at the dial (for comparisons)
    bool rateOn = true;            // off: the shift only, no drift through the frame
    bool loop = true;
    double loopSec = 600;          // the pass repeats after this many seconds of signal
    uint32_t seed = 1;
    LoraSatPass pass;
    std::vector<LoraSatGenFrame> frames;
};

class LoraSatGenerator {
public:
    explicit LoraSatGenerator(const LoraSatGenConfig& c);
    ~LoraSatGenerator();
    void generate(cf32* out, size_t n);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// A beacon every periodSec for the whole pass (signal time = pass time); text payloads with a counter, SF / BW / sync as given
std::vector<LoraSatGenFrame> loraSatBeacons(int sf, double bwHz, int sync, double periodSec, double passSec, int payloadBytes = 24);

std::unique_ptr<ModeSynth> makeLoraSatSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
