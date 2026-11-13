// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SSDV test signal. Nothing is transmitted: the samples only exist in memory or in a file.
// A test picture cut into SSDV packets (ssdv_proto.h) as continuous-phase (optionally Gaussian filtered) 2-FSK, either a plain bit stream or
// RTTY-framed bytes (8N2), with preamble, in white noise, with a carrier offset, a carrier drift and a clock error.
// Wenet (wenet != 0): every packet goes out as a Wenet frame (wenet.h), one after the other.
// SynthConfig (makeSsdvSynth): snrDb is Eb/N0 in dB, cfoHz the carrier offset, modeOpt[0] 0 plain / 1 RTTY-framed, modeVal[0] the baud rate (default 1200).
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/ssdv/ssdv_proto.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct SsdvGenConfig {
    double rate = 2e6;
    double dialOffsetHz = 0;       // where the channel sits in the samples
    double baud = 1200;
    bool async = false;            // RTTY framing: start bit, 8 data bits LSB first, two stop bits
    double deviationHz = 0;        // 0: baud / 2 for plain (MSK-like, at least 1.2 kHz), 1.5 x baud-ish for async (at least 500 Hz)
    double gaussBt = 0;            // 0: rectangular bits; 0.5: GFSK
    double cfoHz = 0;
    double driftHzPerSec = 0;
    double clockPpm = 0;
    double ebn0Db = 99;            // >= 99: no noise
    double amp = 0.4;
    bool invert = false;           // swapped frequencies
    int preambleBytes = 16;
    double gapBytes = 4;           // idle (0x55) bytes between packets
    int wenet = 0;                 // 1: Wenet 115.2 kbaud (serial-word framing, LDPC), 2: Wenet 96 kbaud (raw, scrambled): baud and deviation default to the link's
    int slipEveryBits = 0;         // > 0: one bit is lost every that many bits, < 0: one bit is repeated (clock slips)
    std::vector<std::array<uint8_t, ssdv::kPktSize>> packets;   // sent in order, then again from the start
    uint32_t seed = 1;
};

class SsdvGenerator {
public:
    explicit SsdvGenerator(const SsdvGenConfig& c);
    ~SsdvGenerator();
    void generate(cf32* out, size_t n);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeSsdvSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
