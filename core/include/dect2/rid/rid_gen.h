// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Drone Remote ID test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// The Bluetooth LE and Wi-Fi test generators (ble_gen.h, wifi_gen.h) with Open Drone ID payloads, made at twice the rate, placed where
// ridPlan() expects them for the radio centre (BLE channel 38 6 MHz below 2432 MHz, Wi-Fi channel 6 5 MHz above), summed, low-pass
// filtered and halved to the output rate, as a 20 MHz radio channel would see them.
//
// SynthConfig (makeRidSynth): snrDb, cfoHz; modeOpt[0]: 0 both carriers, 1 only BLE, 2 only Wi-Fi.
#pragma once
#include "dect2/ble/ble_gen.h"
#include "dect2/engine/mode_synth.h"
#include "rid_proto.h"
#include "dect2/wifi/wifi_gen.h"
#include <memory>
#include <vector>

namespace dect2 {

struct RidGenConfig {
    double rate = 20e6;          // output rate
    double centerHz = 2432e6;    // where the radio is (only the offsets of the two carriers follow from it); 0: both at 0 Hz
    double snrDb = 30;           // of each carrier, in its own bandwidth (1 MHz BLE, 20 MHz Wi-Fi); >= 99 no noise
    double cfoHz = 0;
    bool ble = true, wifi = true;
    uint32_t seed = 1;
};

class RidGenerator {
public:
    explicit RidGenerator(const RidGenConfig& c);
    ~RidGenerator();
    void generate(cf32* out, size_t n);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Drone 1 on BLE (random static address C6:12:34:56:78:9A): Basic ID serial "1581F5FKD229400B" and session ID, multirotor, airborne at
// 54.6872 N 25.2797 E, 120 m height, 8.5 m/s heading 90, operator at 54.6860 N 25.2770 E, operator ID "LTUabcd1234efgh", Self-ID text,
// one authentication page; and a BLE beacon without Remote ID.
std::vector<rid::Message> ridTestDrone1();
// Drone 2 on Wi-Fi (source 60:60:1F:AA:BB:CC, SSID "RID-DJI-2"): a message pack with Basic ID (CAA "FIN87astrdge12k8"), aeroplane, location,
// system and operator ID; and a Wi-Fi beacon without Remote ID.
std::vector<rid::Message> ridTestDrone2();
std::vector<BleGenPacket> ridBleSchedule();
std::vector<WifiGenFrame> ridWifiSchedule();

std::unique_ptr<ModeSynth> makeRidSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
