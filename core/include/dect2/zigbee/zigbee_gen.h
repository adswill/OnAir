// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Zigbee / IEEE 802.15.4 test signal (2.4 GHz O-QPSK). Nothing is transmitted: the samples only exist in memory or in a file.
//
// A schedule of frames (zigbee_proto.h builds them): preamble (8 symbols of 0), SFD 0xA7, PHR, PSDU; every 4-bit symbol is spread to its
// 32-chip sequence, 2 Mchip/s, the even chips on I, the odd on Q delayed by one chip, half-sine pulses (constant envelope, MSK
// equivalent); separated by gaps of silence, over complex white noise. The chip clock and the start of each frame are not locked to
// the sample clock (a random fraction of a sample, and clockPpm).
//
// SynthConfig (makeZigbeeSynth): snrDb (carrier to noise in 2 MHz), cfoHz; modeOpt[0]: 0 the schedule.
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct ZigbeeGenFrame {
    std::vector<uint8_t> psdu;   // MAC frame with the FCS (zigbee::buildMac)
    double gapSec = 0.002;       // silence after it
    double ampScale = 1;
    double cfoHz = 0;            // this device's own carrier offset, on top of the config's
    bool badFcs = false;         // tests: the last symbol of the PSDU sent as another one
    int preambleSyms = 8;        // tests: a shorter preamble
};

struct ZigbeeGenConfig {
    double rate = 4e6;
    double dialOffsetHz = 0;
    double snrDb = 30;           // carrier to noise in 2 MHz; >= 99: no noise
    double cfoHz = 0;
    double clockPpm = 0;         // the transmitter's chip clock against the receiver's sample clock
    double amp = 0.3;
    double leadSec = 0.0005;
    bool loop = true;
    uint32_t seed = 1;
    double interfererDb = -200;  // an adjacent-channel transmitter (continuous O-QPSK of random data) this much above the frames' level (dB); <= -100: none
    double interfererHz = 5e6;   // where it lies from the wanted channel (needs a sample rate above twice this)
    std::vector<ZigbeeGenFrame> frames;
};

class ZigbeeGenerator {
public:
    explicit ZigbeeGenerator(const ZigbeeGenConfig& c);
    ~ZigbeeGenerator();
    void generate(cf32* out, size_t n);
    static double noiseSigma(const ZigbeeGenConfig& c);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The chips of a PPDU (preamble, SFD, PHR, PSDU), one value (0 / 1) per entry, c0 first
std::vector<uint8_t> zigbeeChips(const std::vector<uint8_t>& psdu, int preambleSyms = 8);

// A coordinator beaconing, devices joining and reporting, encrypted and clear frames: vendors, a Zigbee beacon payload, MAC commands,
// NWK headers, APS clusters, encrypted payloads
std::vector<ZigbeeGenFrame> zigbeeTestSchedule();

std::unique_ptr<ModeSynth> makeZigbeeSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
