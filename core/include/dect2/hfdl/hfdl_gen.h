// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HFDL test signal: ground stations and aircraft on HFDL channels, PSK bursts (root raised cosine, roll-off 0.35, 1800 baud) with the
// real burst structure (prekey, A1, A2, M1, M2, training, data blocks with probes, scrambled, interleaved, K = 7 coded). Nothing is
// transmitted; the samples only exist in memory or in a file. Synthetic content: flights, registrations and texts are made up.
//
// Traffic: every ground station sends a squitter (SPDU) in slot 0 of every 32 s frame; aircraft log on (logon request, logon confirm),
// then send performance data and frequency data (with their position) and ACARS downlinks, and get ACARS uplinks. Every data rate is used.
//
// Through the engine (SynthConfig, mode 37):
//   modeOpt[0]  number of aircraft, 0 = 6 (1 to 24)
//   modeOpt[1]  random seed, 0 = 1
//   modeOpt[2]  HF channel: 0 none, 1 two paths (1 ms, -3 dB) with 0.5 Hz Doppler spread, 2 two paths (2 ms, -1 dB) with 1 Hz spread
//   modeVal[0]  traffic rate factor, 0 = 1
// snrDb: Es/N0 of a 0 dB burst. cfoHz and sroPpm shift every burst. Centre 8.9 MHz; channels 8.927 (San Francisco), 8.942 (Shannon),
// 8.977 MHz (Reykjavik).
#pragma once
#include "hfdl_proto.h"
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

struct HfdlPath { double delaySec = 0; double gainDb = 0; double spreadHz = 0; double shiftHz = 0; };

// one burst as a waveform at 8 samples per symbol around 0 Hz, ready to be placed into a stream
struct HfdlWave {
    double start = 0;            // output sample of the first prekey symbol (fractional)
    double carHz = 0;            // carrier relative to 0 Hz of the output
    double amp = 0.1;
    double phase = 0;
    std::vector<cf32> bb;        // 8 samples per symbol; bb[0] is 6 symbols before the first symbol
    double endSample(double rate) const;
};
HfdlWave hfdlMakeWave(int m1, const std::vector<uint8_t>& pdu, const std::vector<HfdlPath>& paths = {}, uint32_t seed = 1);
void hfdlRender(const HfdlWave& w, cf32* out, int64_t outStart, size_t n, double rate);
double hfdlBurstSeconds(int m1);

struct HfdlSentPdu { double startSec = 0; double freqHz = 0; int m1 = 0; std::vector<uint8_t> pdu; };
struct HfdlSentLog { std::mutex mu; std::vector<HfdlSentPdu> sent; };

struct HfdlGenOptions {
    int aircraft = 6;
    uint32_t seed = 1;
    double centerHz = 8.9e6;
    std::vector<double> channelsHz = {8.927e6, 8.942e6, 8.977e6};   // dial frequencies
    std::vector<int> gsIds = {1, 7, 3};
    double rateFactor = 1;
    std::vector<HfdlPath> paths;                // empty: one clean path
    double cfoSpreadHz = 30;                    // each aircraft's own offset, up to this
    std::shared_ptr<HfdlSentLog> log;
    std::string text;                           // nonempty: ACARS H1 downlinks carry this
    std::string reg;                            // nonempty: the first aircraft uses this registration
};

struct HfdlTxBurst { double t = 0; int chan = 0; int m1 = 1; std::vector<uint8_t> pdu; double levelDb = 0; double carOffHz = 0; };
class HfdlTraffic {
public:
    explicit HfdlTraffic(const HfdlGenOptions& o);
    ~HfdlTraffic();
    HfdlTxBurst next();          // in time order
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeHfdlSynthEx(const HfdlGenOptions& o, const SynthConfig& cfg, double sampleRate);
std::unique_ptr<ModeSynth> makeHfdlSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
