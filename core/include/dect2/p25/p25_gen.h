// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// P25 Phase 1 test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// A schedule of transmissions: a trunked site's control channel (TSBKs: network, RFSS and adjacent site status, identifier updates,
// group voice grants), voice calls (HDU, LDU1 / LDU2 pairs with the link control and encryption sync, TDULC), an encrypted call and a
// data packet header; C4FM (raised cosine 0.2, +-1800 Hz for +-3) over complex white noise. The voice bits are a known pattern
// (p25KnownVoice): there is no free IMBE encoder.
//
// SynthConfig (makeP25Synth):
//   snrDb       carrier to noise in 12.5 kHz
//   cfoHz       the radio's tuning error
//   modeOpt[0]  0 the test schedule, 1 only the control channel, 2 only the voice calls
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

struct P25GenTx {
    enum Kind { Control, Voice, Data } kind = Voice;
    int nac = 0x293;
    // control channel
    int wacn = 0xBEE00, sysId = 0x3A1, rfss = 1, site = 5, ctrlChannel = 0x1001;
    int adjSite = 6, adjChannel = 0x1009;
    double idenBaseHz = 851006250, idenSpacingHz = 12500;   // identifier 1
    // a control channel of the test's own making: one entry per frame (up to three TSBKs: opcode, 64 argument bits), sent in turn for durSec.
    // Empty: the built-in schedule above.
    std::vector<std::vector<std::pair<int, uint64_t>>> script;
    int grantTg = 1201, grantSrc = 1234567, grantChannel = 0x1010;
    // voice
    int src = 1234567, dst = 1201;
    bool group = true, emergency = false;
    int algId = 0x80, keyId = 0;
    bool noHeader = false;       // start with the first LDU1 (a receiver joining late)
    bool noTerm = false;         // the carrier just stops
    // data
    int llid = 1234567;
    double durSec = 2;
    double gapSec = 0.6;
};

struct P25GenConfig {
    double rate = 2e6;
    double dialOffsetHz = 0;     // where the channel sits in the samples
    double snrDb = 30;           // carrier to noise in 12.5 kHz; >= 99: no noise
    double cfoHz = 0;
    double clockPpm = 0;         // symbol clock error of the transmitter
    double amp = 0.05;
    double leadSec = 0.3;
    bool loop = true;
    uint32_t seed = 1;
    std::vector<P25GenTx> txs;
};

class P25Generator {
public:
    explicit P25Generator(const P25GenConfig& c);
    ~P25Generator();
    void generate(cf32* out, size_t n);
    double timeSec() const;
    static double noiseSigma(const P25GenConfig& c);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The C4FM symbols (-3 -1 +1 +3) of one transmission
void p25TxSymbols(const P25GenTx& tx, std::vector<int8_t>& sym);
void p25KnownVoice(uint32_t fn, uint8_t out[144]);   // the IMBE bits (c0..c7) of voice frame fn
std::vector<P25GenTx> p25TestSchedule();

std::unique_ptr<ModeSynth> makeP25Synth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
