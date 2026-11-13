// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// dPMR test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// A schedule of transmissions: voice calls (header, superframes of CCH + four voice frames, end frame), status messages, short messages
// (data type 2) and packet data (FS4 header); four-level FSK with a raised cosine (0.2) pulse over complex white noise. The voice bits
// are a known pattern (dpmrKnownVoice): there is no free AMBE+2 encoder.
//
// SynthConfig (makeDpmrSynth): snrDb (carrier to noise in 6.25 kHz), cfoHz.
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct DpmrGenTx {
    enum Kind { Voice, Status, Message, Packet } kind = Voice;
    uint32_t own = 1001, called = 200;
    int callType = 1, cc = 7, status = 0;
    bool emergency = false;
    std::string text;
    double durSec = 2;
    double gapSec = 0.8;
    double chOffsetHz = 0;       // where in the band (the PMR446 channel raster: n x 6250 Hz)
    bool noHeader = false;       // voice: start with the payload frames (a receiver joining late)
    bool noEnd = false;          // voice: the carrier just stops
};

struct DpmrGenConfig {
    double rate = 2e6;
    double dialOffsetHz = 0;
    double snrDb = 30;           // carrier to noise in 6.25 kHz; >= 99: no noise
    double cfoHz = 0;
    double clockPpm = 0;
    double symErrRate = 0;       // fraction of the non-sync symbols sent wrong (a level step)
    double amp = 0.05;
    double leadSec = 0.3;
    bool loop = true;
    uint32_t seed = 1;
    std::vector<DpmrGenTx> txs;
};

class DpmrGenerator {
public:
    explicit DpmrGenerator(const DpmrGenConfig& c);
    ~DpmrGenerator();
    void generate(cf32* out, size_t n);
    static double noiseSigma(const DpmrGenConfig& c);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The symbols of one transmission, preamble to end frame; isSync marks the frame sync symbols
void dpmrTxSymbols(const DpmrGenTx& tx, std::vector<int8_t>& sym, std::vector<uint8_t>* isSync = nullptr);
void dpmrKnownVoice(uint32_t fn, uint8_t out[72]);
std::vector<DpmrGenTx> dpmrTestSchedule();

std::unique_ptr<ModeSynth> makeDpmrSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
