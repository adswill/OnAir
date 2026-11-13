// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// D-STAR test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// A schedule of calls, each a whole DV transmission: bit sync, frame sync, radio header, voice frames with slow data (the message, the
// header resend and GPS text, repeated), end pattern; GMSK at 4800 bit/s (BT 0.5, +-1.2 kHz) over complex white noise. The voice bits are
// a known pattern (dstarKnownVoice): there is no free AMBE encoder.
//
// SynthConfig (makeDstarSynth):
//   snrDb       carrier to noise in 6.25 kHz
//   cfoHz       the radio's tuning error
//   modeOpt[0]  0 the test schedule (dstarTestSchedule), 1 only its first call
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dstar_proto.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct DstarGenTx {
    dstar::Header hdr;
    std::string text;            // the slow data message (padded or cut to 20 characters); "" none
    std::vector<std::string> gps;   // GPS lines sent in the slow data, each ending in "\r\n"
    bool headerResend = true;    // the header in the slow data too
    int squelch = -1;            // digital code squelch, -1 none
    double durSec = 2;           // voice (frames of 20 ms)
    double gapSec = 1;           // silence after it
    int headerErrors = 0;        // channel bit errors put into the 660 header bits (spread over them)
    int dropBlock = 0;           // in the first round of the slow data, every dropBlock-th block is sent as filler (a block lost); 0 none
    bool endPattern = true;      // false: the carrier just stops
};

struct DstarGenConfig {
    double rate = 2e6;
    double dialOffsetHz = 0;     // where the channel sits in the samples (-tuneOffsetHz for the radio, 0 for a recording)
    double snrDb = 30;           // carrier to noise in 6.25 kHz; >= 99: no noise
    double cfoHz = 0;
    double amp = 0.05;
    double leadSec = 0.3;
    bool loop = true;
    uint32_t seed = 1;
    std::vector<DstarGenTx> txs;
};

class DstarGenerator {
public:
    explicit DstarGenerator(const DstarGenConfig& c);
    ~DstarGenerator();
    void generate(cf32* out, size_t n);
    double timeSec() const;
    static double noiseSigma(const DstarGenConfig& c);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The bits of one whole transmission, from the bit sync to the end pattern
void dstarTxBits(const DstarGenTx& tx, std::vector<uint8_t>& bits, uint32_t seed = 1);
// The voice bits of frame fn (no AMBE encoder exists here)
void dstarKnownVoice(uint32_t fn, uint8_t out[72]);
std::string dstarRmc(double lat, double lon, double speedKmh, int course);   // "$GPRMC,...*hh\r\n"
std::string dstarGga(double lat, double lon, double altM);
std::string dstarDprs(const std::string& call, double lat, double lon, const std::string& comment);
std::vector<DstarGenTx> dstarTestSchedule();

std::unique_ptr<ModeSynth> makeDstarSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
