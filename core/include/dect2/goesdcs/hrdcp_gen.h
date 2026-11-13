// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meteosat HRDCP test signal: platforms on the 402.2 - 402.435 MHz regional band, each sending its message now and then: 2 s carrier, preamble and
// marker as BPSK, the coded message as offset QPSK (hrdcp_proto.h), root raised cosine roll-off 0.5, with frequency, drift and clock errors and noise.
// Nothing is transmitted: the samples only exist in memory or a file.
//
// SynthConfig (makeHrdcpSynth): snrDb is Eb/N0 of the platforms (dB, per information bit at 1200 bit/s), cfoHz adds a frequency error;
// modeOpt[0]: 0 two platforms (a short self-timed report and a 2-frame one), 1 one short report every 12 s.
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct HrdcpGenTx {
    double freqHz = 402.3e6;            // absolute carrier frequency
    uint32_t address21 = 0x2C412;       // the 21 address bits (the BCH check is added)
    uint16_t sequence = 1;              // counts up by one with every repeat
    int type = 0, health = 0x155;
    std::vector<uint8_t> data;          // platform data
    double startSec = 0.5;
    double repeatSec = 0;               // > 0: sends again this often
    double carrierSec = 2.0;
    double ppm = 0;                     // symbol clock error
    double extraHz = 0;                 // frequency error of the transmitter
    double driftHzPerSec = 0;           // frequency drift during the message
    double phaseNoiseDeg = 0;           // random-walk phase noise, degrees rms per second
    double amp = 0.1;                   // rms of the signal
    int tailBytes = 3;                  // flush bytes sent during the ramp down
};

struct HrdcpGenConfig {
    double rate = 250000;
    double centreHz = 402.3e6;
    double ebn0Db = 12;                 // of a platform with amp 0.1; >= 99: no noise
    uint32_t seed = 1;
    std::vector<HrdcpGenTx> tx;
};

class HrdcpGenerator {
public:
    explicit HrdcpGenerator(const HrdcpGenConfig& c);
    ~HrdcpGenerator();
    void generate(cf32* out, size_t n);
    double burstSeconds(size_t txIndex) const;   // length of one message of that platform on the air
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeHrdcpSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
