// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-SUN FAN test signal (IEEE 802.15.4g SUN FSK). Nothing is transmitted: the samples only exist in memory or in a file.
//
// A schedule of frames (wisun_proto.h builds them), each on its own channel of the band: preamble (0x55 octets), the start-of-frame delimiter
// 0x904E, the PHR, the PSDU (FCS added, PN9 whitened); 2-GFSK (BT 0.5) at the frame's bit rate and modulation index, a continuous phase;
// separated by gaps of silence, over complex white noise across the whole sample rate. The bit clock and the start of each frame are not
// locked to the sample clock (clockPpm and the gaps).
//
// SynthConfig (makeWisunSynth): snrDb (carrier to noise in 400 kHz), cfoHz.
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct WisunGenFrame {
    std::vector<uint8_t> mpdu;   // MAC frame without the FCS (wisun::buildMpdu)
    double mhz = 916.2;          // the channel
    int bitRate = 150000;
    double modIndex = 0.5;
    double gapSec = 0.004;       // silence after it
    double ampScale = 1;
    double cfoHz = 0;            // this node's own carrier offset, on top of the config's
    int preambleOctets = 8;
    bool fcs2 = false;
    bool whiten = true;
    bool badFcs = false;         // tests: the last FCS octet sent wrong
    bool msError = false;        // tests: the PHR's mode switch bit sent set (as a bit error makes it)
};

struct WisunGenConfig {
    double rate = 20e6;
    double centerMhz = 921.5;    // the radio's centre: where the channels lie in the samples
    double snrDb = 30;           // carrier to noise in 400 kHz; >= 99: no noise
    double cfoHz = 0;
    double clockPpm = 0;         // the transmitters' bit clock against the receiver's sample clock
    double amp = 0.3;
    double leadSec = 0.002;
    bool loop = true;
    uint32_t seed = 1;
    std::vector<WisunGenFrame> frames;
};

class WisunGenerator {
public:
    explicit WisunGenerator(const WisunGenConfig& c);
    ~WisunGenerator();
    void generate(cf32* out, size_t n);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The bits of a PPDU as sent (preamble, delimiter, PHR, whitened PSDU with its FCS), first bit first
std::vector<uint8_t> wisunPpduBits(const WisunGenFrame& f);

// A small FAN 1.1 network on the 915 - 928 MHz ANZ channels: a border router and a router advertising, a node soliciting and joining (EAPOL
// with the authenticator), PAN configurations and data (encrypted), unicast data with enhanced acks reporting the level (RSL), a 50 kbit/s
// frame on the 200 kHz plan; the frames hop over the channels around centerMhz within +-span
std::vector<WisunGenFrame> wisunTestSchedule(double centerMhz = 921.5, double spanMhz = 6.0);

std::unique_ptr<ModeSynth> makeWisunSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
