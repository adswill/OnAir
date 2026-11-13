// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi beacon test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// A schedule of 802.11 frames (wifi_proto.h builds them), each sent either as a long-preamble 1 Mbit/s DSSS PPDU (DBPSK, Barker-11 at
// 11 Mchip/s, rectangular chips: the 2.4 GHz beacons) or as a non-HT OFDM PPDU at 6 Mbit/s (wifi_ofdm.h: the 5 and 6 GHz beacons, FILS discovery
// and probe responses), separated by gaps of silence, over complex white noise. An OFDM frame can sit anywhere in a wide capture (offHz)
// and be repeated on other 20 MHz channels at once (dupHz: the non-HT duplicate of a 40 / 80 / 160 MHz BSS).
//
// SynthConfig (makeWifiSynth): snrDb (signal to noise in 20 MHz), cfoHz; modeOpt[0]: 0 2.4 GHz (DSSS), 1 5 GHz, 2 6 GHz; modeOpt[1]: the channel
// (0: 36 on 5 GHz, 5 on 6 GHz).
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct WifiGenFrame {
    std::vector<uint8_t> mpdu;   // FCS included (wifi::makeBeacon)
    double gapSec = 0.002;       // silence after it
    double ampScale = 1;
    double cfoHz = 0;            // this AP's own carrier offset, on top of the config's
    bool badFcs = false;         // tests: one bit of the FCS sent wrong
    int phy = 0;                 // 0 DSSS 1 Mbit/s, 1 OFDM 6 Mbit/s, 2 DSSS 2 Mbit/s long preamble, 3 DSSS 2 Mbit/s short preamble, 4 OFDM 12 Mbit/s
    double offHz = 0;            // where the channel sits relative to the capture's centre (a wide capture)
    std::vector<double> dupHz;   // OFDM: further offsets at which the same frame is sent at the same time (non-HT duplicate)
    uint8_t scrSeed = 0x5D;      // OFDM: the scrambler start state
};

struct WifiGenConfig {
    double rate = 20e6;
    double snrDb = 30;           // signal to noise in 20 MHz; >= 99: no noise
    double cfoHz = 0;
    double clockPpm = 0;
    double amp = 0.25;
    double leadSec = 0.001;
    bool loop = true;
    uint32_t seed = 1;
    std::vector<WifiGenFrame> frames;
};

class WifiGenerator {
public:
    explicit WifiGenerator(const WifiGenConfig& c);
    ~WifiGenerator();
    void generate(cf32* out, size_t n);
    static double noiseSigma(const WifiGenConfig& c);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The 5 / 6 GHz scene on one channel (primary 20 MHz channel `channel` of band 1 or 2): APs of every generation with beacons, a probe response and (6 GHz)
// FILS discovery frames and an unsolicited probe response; the 5 GHz scene also carries an RNR naming a co-located 6 GHz AP. All OFDM frames.
std::vector<WifiGenFrame> wifiTestSchedule56(int band, int channel);

// Two APs on channel 6 ("OnAir-Home", WPA2, AVM; "OnAir-Cafe", WPA2/WPA3, Ubiquiti), a hidden open AP, a probe response and a data frame.
std::vector<WifiGenFrame> wifiTestSchedule();

std::unique_ptr<ModeSynth> makeWifiSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
