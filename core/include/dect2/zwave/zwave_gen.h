// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Z-Wave test signal. Nothing is transmitted: the samples only exist in memory or in a file.
// A small network (home ID C0FFEE01, controller node 1, nodes 2 - 5) on the EU channels, one frame at a time with gaps of silence: R1 (Manchester,
// 9.6 kbit/s) and R2 (40 kbit/s) frames with a switch, a dimmer, a temperature sensor, a battery report, a wake-up, a node info frame, acks, a multicast
// and an S0 and an S2 encrypted frame (ciphertext stands in as random octets: nothing is encrypted for real), R3 (100 kbit/s GFSK) frames with CRC-16;
// over complex white noise across the whole sample rate. SynthConfig: snrDb (carrier to noise in 100 kHz), cfoHz; modeOpt[0] = region index in zwave::regions().
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/zwave/zwave_proto.h"
#include <memory>
#include <vector>

namespace dect2 {

struct ZwGenFrame {
    std::vector<uint8_t> mpdu;   // with the checksum / CRC (zwave::build)
    int rate = 2;
    double mhz = 868.40;
    double gapSec = 0.012;
    double ampScale = 1;
};

struct ZwGenConfig {
    double rate = 2e6;
    double centerMhz = 869.13;
    double snrDb = 30;           // carrier to noise in 100 kHz; >= 99: no noise
    double cfoHz = 0;
    double clockPpm = 0;
    double amp = 0.3;
    uint32_t seed = 1;
    bool loop = true;
    std::vector<ZwGenFrame> frames;
};

// The bits as sent: preamble octets of 0x55, the delimiter, the MPDU, MSB first (R1: before Manchester coding)
std::vector<uint8_t> zwavePpduBits(const ZwGenFrame& f);
std::vector<ZwGenFrame> zwaveTestSchedule(const std::string& region = "EU");

class ZwaveGenerator {
public:
    explicit ZwaveGenerator(const ZwGenConfig& c);
    ~ZwaveGenerator();
    void generate(cf32* out, size_t n);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeZwaveSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
