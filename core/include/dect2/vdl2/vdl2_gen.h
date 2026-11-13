// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VDL Mode 2 test signal: aircraft and ground stations on the VDL2 channels, D8PSK bursts (raised cosine, roll-off 0.6) with the real
// burst structure (ramp, training sequence, header, scrambled Reed-Solomon coded data, AVLC frames). Nothing is transmitted; the samples
// only exist in memory or in a file. Synthetic content: registrations, flights and texts are made up in the style of real ones.
//
// Traffic: each ground station sends a GSIF (its location and airports) every few seconds; each aircraft first sends a link
// establishment XID (location, altitude, destination), then ACARS downlinks (free text, ADS-C basic reports, label 16 positions, FANS-1/A
// CPDLC), X.25 call requests and X.25 data packets with CLNP / COTP headers; the ground station answers with RR frames and ACARS uplinks.
// Bursts on one channel do not overlap (the channel is sensed first, as VDL2's CSMA does).
//
// Through the engine (SynthConfig, mode 29):
//   modeOpt[0]  number of aircraft, 0 = 6 (1 to 24)
//   modeOpt[1]  random seed, 0 = 1
//   modeOpt[2]  channels: 0 = the European six (136.725 - 136.975), 1 = 136.975 only
//   modeVal[0]  traffic rate factor, 0 = 1 (each aircraft sends about every 6 s divided by this)
//   modeVal[1]  extra attenuation of the last channel in dB (a weak channel next to strong ones)
// snrDb: Es/N0 of a 0 dB burst (signal power over the noise in 10.5 kHz). cfoHz and sroPpm shift every burst. Centre 136.85 MHz.
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

using Vdl2Frames = std::vector<std::vector<uint8_t>>;   // AVLC frames, each with its FCS

// One burst as a waveform: where it starts (input samples, fractional), its carrier (Hz from 0), amplitude, phase, and symbols
struct Vdl2Wave {
    double start = 0;
    double carHz = 0;
    double amp = 0.08;
    double phase = 0;
    std::vector<int8_t> ph;       // absolute phase of each symbol in units of pi/4 (ramp, training, data, tail)
    std::vector<float> env;       // amplitude of each symbol (ramps)
    double endSample(double rate) const;
};
Vdl2Wave vdl2MakeWave(const Vdl2Frames& frames);              // symbols for these frames (start, carrier, amplitude left to the caller)
double vdl2BurstSeconds(const Vdl2Frames& frames);
// Adds the part of the burst that falls into out[0 .. n) where out[0] is input sample outStart. rate: samples per second.
void vdl2Render(const Vdl2Wave& w, cf32* out, int64_t outStart, size_t n, double rate);

struct Vdl2SentFrame { double startSec = 0; double freqHz = 0; std::vector<uint8_t> frame; };
struct Vdl2SentLog { std::mutex mu; std::vector<Vdl2SentFrame> sent; };

struct Vdl2GenOptions {
    int aircraft = 6;
    uint32_t seed = 1;
    double centerHz = 136.85e6;
    std::vector<double> channelsHz = {136.725e6, 136.775e6, 136.825e6, 136.875e6, 136.900e6, 136.975e6};
    std::vector<double> levelDb;                // extra level per channel (missing = 0)
    double rateFactor = 1;
    double cfoSpreadHz = 150;                   // each transmitter's own offset, up to this
    double levelSpreadDb = 10;                  // aircraft are 0 to this many dB below the ground stations
    double guardSec = 0.004;                    // quiet time between bursts on one channel
    std::shared_ptr<Vdl2SentLog> log;
    std::string text;                           // nonempty: ACARS free-text downlinks carry this
    std::string reg;                            // nonempty: the first aircraft uses this registration
};

// The traffic alone (tests): bursts in time order
struct Vdl2TxBurst { double t = 0; int chan = 0; Vdl2Frames frames; double levelDb = 0; double carOffHz = 0; };
class Vdl2Traffic {
public:
    explicit Vdl2Traffic(const Vdl2GenOptions& o);
    ~Vdl2Traffic();
    Vdl2TxBurst next();
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeVdl2SynthEx(const Vdl2GenOptions& o, const SynthConfig& cfg, double sampleRate);
std::unique_ptr<ModeSynth> makeVdl2Synth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
