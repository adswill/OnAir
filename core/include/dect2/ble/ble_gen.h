// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Bluetooth LE advertising test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// A schedule of advertising packets (ble_proto.h builds them), each GFSK modulated (BT 0.5, h 0.5, 1 Msym/s) on its channel's whitening,
// separated by gaps of silence, over complex white noise.
//
// SynthConfig (makeBleSynth): snrDb (carrier to noise in 1 MHz), cfoHz; modeOpt[0]: 0 the packets on channels 37, 38 and 39 in turn,
// 1 / 2 / 3 only channel 37 / 38 / 39.
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct BleGenPacket {
    std::vector<uint8_t> pdu;    // header + payload (ble::makePdu)
    int channel = 37;            // the whitening (0 to 39)
    double gapSec = 0.003;       // silence after it
    double ampScale = 1;
    double cfoHz = 0;            // this device's own carrier offset, on top of the config's
    bool badCrc = false;         // tests: the last CRC bit sent wrong
};

struct BleGenConfig {
    double rate = 4e6;
    double dialOffsetHz = 0;
    double snrDb = 30;           // carrier to noise in 1 MHz; >= 99: no noise
    double cfoHz = 0;
    double clockPpm = 0;
    double amp = 0.3;
    double leadSec = 0.002;
    bool loop = true;
    int channel = 0;             // 0: each packet's own; 37 / 38 / 39: every packet whitened for that one
    uint32_t seed = 1;
    std::vector<BleGenPacket> pkts;
};

class BleGenerator {
public:
    explicit BleGenerator(const BleGenConfig& c);
    ~BleGenerator();
    void generate(cf32* out, size_t n);
    static double noiseSigma(const BleGenConfig& c);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Phones, beacons, a scanner and an initiator: names, manufacturer data, iBeacon, Eddystone UID / URL / TLM, exposure notification,
// SCAN_REQ / SCAN_RSP, CONNECT_IND and ADV_EXT_IND. channel 0: the packets go round 37, 38, 39.
std::vector<BleGenPacket> bleTestSchedule(int channel = 0);

std::unique_ptr<ModeSynth> makeBleSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
