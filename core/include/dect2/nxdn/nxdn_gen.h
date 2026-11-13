// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NXDN test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// A schedule of transmissions at either rate: a trunked site's control channel (CAC with SITE_INFO / SRV_INFO), voice calls (FACCH1
// VCALL header, superframes of SACCH VCALL + four voice frames, FACCH1 TX_REL), status and short data calls; four-level FSK with a
// raised cosine (0.2) pulse over complex white noise. The voice bits are a known pattern (nxdnKnownVoice): there is no free AMBE+2 encoder.
//
// SynthConfig (makeNxdnSynth):
//   snrDb       carrier to noise in 12.5 kHz
//   cfoHz       the radio's tuning error
//   modeOpt[0]  0 the test schedule (both rates), 1 only its NXDN48 transmissions, 2 only NXDN96
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct NxdnGenTx {
    enum Kind { Voice, Status, Data, Site } kind = Voice;
    int baud = 2400;
    int src = 1001, dst = 200, callType = 1, ran = 1;
    int cipher = 0;
    int sacchGlitchCipher = 0;   // test: one SACCH superframe (the 3rd) carries this cipher type with a valid CRC (a misparsed message)
    int status = 0;
    std::string text;            // short data
    double durSec = 2;           // voice / site
    double gapSec = 0.8;
    bool noHeader = false;       // voice: start with the first superframe (a receiver joining late)
    bool noRelease = false;      // voice: the carrier just stops
    uint32_t location = 0x012345;
};

struct NxdnGenConfig {
    double rate = 2e6;
    double dialOffsetHz = 0;     // where the channel sits in the samples
    double snrDb = 30;           // carrier to noise in 12.5 kHz; >= 99: no noise
    double cfoHz = 0;
    double clockPpm = 0;         // symbol clock error of the transmitter
    double symErrRate = 0;       // fraction of the payload symbols sent wrong (a level step), before the pulse shaping
    double amp = 0.05;
    double leadSec = 0.3;
    bool loop = true;
    uint32_t seed = 1;
    std::vector<NxdnGenTx> txs;
};

class NxdnGenerator {
public:
    explicit NxdnGenerator(const NxdnGenConfig& c);
    ~NxdnGenerator();
    void generate(cf32* out, size_t n);
    double timeSec() const;
    static double noiseSigma(const NxdnGenConfig& c);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The symbols (-3 -1 +1 +3) of one transmission, preamble to the last frame
void nxdnTxSymbols(const NxdnGenTx& tx, std::vector<int8_t>& sym);
void nxdnKnownVoice(uint32_t fn, uint8_t out[72]);   // the voice bits of voice frame fn
std::vector<NxdnGenTx> nxdnTestSchedule();

std::unique_ptr<ModeSynth> makeNxdnSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
