// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GOES DCS test signal: platforms on their 1.5 kHz channels of the 401.7 - 402.1 MHz uplink band, each sending its report now and then: unmodulated carrier,
// preamble, frame sync, address, ASCII or pseudo-binary data and the EOT code, carrier phase modulated +-60 degrees with NRZ or Manchester bits, in noise.
// Nothing is transmitted: the samples only exist in memory or a file.
//
// SynthConfig (makeGoesDcsSynth): snrDb (carrier over the noise in 100 Hz), cfoHz (added frequency error of the platforms, default 0);
// modeOpt[0]: 0 three platforms (100 bps, 300 bps and 1200 bps Manchester) each sending every ~16 s, 1 only the 300 bps platform, 2 one platform of each
//             rate with 300 and 1200 bps as NRZ; modeOpt[1]: 1 adds a steady unmodulated carrier on a fourth channel (must be ignored).
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct DcsTruth { double startSec; int channel; uint32_t address21; int bps; bool manchester; std::string text; bool pseudo; };

struct GoesDcsGenTx {
    int channel = 100;                  // on the GOES grid
    double freqHz = 0;                  // > 0: this absolute carrier frequency instead (Meteosat, Himawari channels)
    uint32_t address21 = 0x12345;
    int bps = 300;
    bool manchester = true;
    bool ext = true;                    // the spare bit and flag word (300 / 1200 bps format)
    uint8_t flags = 0x21;
    double startSec = 1.0;
    double carrierSec = 0.6;            // unmodulated carrier before the preamble
    std::vector<uint8_t> data;          // 7-bit characters (the parity is added) without the EOT
    double repeatSec = 0;               // > 0: sends again this often (data unchanged)
    double ppm = 0;                     // bit clock error
    double extraHz = 0;                 // frequency error of the transmitter
    double amp = 0.3;
    bool preamble = true;
};

struct GoesDcsGenConfig {
    double rate = 1e6;
    double centreHz = 401.85e6;
    double snrDb = 40;                  // >= 99: no noise
    uint32_t seed = 1;
    std::vector<GoesDcsGenTx> tx;
    std::vector<int> carrierChannels;   // steady unmodulated carriers
};

class GoesDcsGenerator {
public:
    explicit GoesDcsGenerator(const GoesDcsGenConfig& c);
    ~GoesDcsGenerator();
    void generate(cf32* out, size_t n);
    double offsetHz(int channel) const;       // from the radio centre
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeGoesDcsSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
