// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi HaLow (802.11ah) test signal, sub-GHz S1G OFDM. Nothing is transmitted: the samples only exist in memory or in a file.
//
// A schedule of PPDUs (halow_phy.h builds them: STF, LTF1, SIG, data at any MCS) at 1, 2, 4, 8 and 16 MHz with the S1G_1M, S1G_SHORT or S1G_LONG
// preamble, separated by gaps of silence, over complex white noise. Each PPDU is made on its own tone grid (4 / 8 / 16 Msps) and brought to the radio's
// sample rate with a windowed sinc; the start of each PPDU and the sample clock (clockPpm) are not locked to the receiver's. Impairments: carrier offset,
// the signal off the centre of the capture (dialOffsetHz), clock error, noise, a one-echo multipath, starting the stream in the middle of a PPDU.
//
// SynthConfig (makeHalowSynth): snrDb (signal to noise in 1 MHz), cfoHz; modeOpt[0]: 0 the schedule.
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct HalowGenFrame {
    std::vector<uint8_t> psdu;   // MAC frame with its FCS (halow::makeS1gBeacon ...)
    int width = 0;               // 0 1 MHz, 1 2 MHz, 2 4 MHz, 3 8 MHz, 4 16 MHz
    int mcs = 0;                 // 0 - 9, or 10 (1 MHz); a combination the width cannot send gives the preamble and SIG field only
    int pre = -1;                // -1: S1G_1M at 1 MHz, S1G_SHORT otherwise; 1: S1G_LONG
    int nss = 1;                 // > 1, stbc, ldpc, agg: the SIG field announces it and the data field is filler (the monitor reads only the SIG field)
    bool stbc = false, ldpc = false, sgi = false, agg = false, ndp = false, beamformed = false;
    double beamTilt = 0;         // beamformed: phase slope across the tones applied after the omni preamble
    double gapSec = 0.003;       // silence after it
    double ampScale = 1;
    double cfoHz = 0;            // this transmitter's own carrier offset, on top of the config's
    uint8_t seed = 0x5D;         // scrambler state
    bool badFcs = false;         // tests: one bit of the PSDU flipped after the FCS was computed
};

struct HalowGenConfig {
    double rate = 4e6;
    double dialOffsetHz = 0;
    double snrDb = 30;           // signal to noise in 1 MHz; >= 99: no noise
    double cfoHz = 0;
    double clockPpm = 0;         // the transmitter's sample clock against the receiver's
    double amp = 0.3;
    double leadSec = 0.0005;
    bool loop = true;
    uint32_t seed = 1;
    double echoDelaySec = 0;     // multipath: one echo this late (up to the 8 us guard interval) ...
    double echoGainDb = -99;     // ... this much weaker (amplitude), with a random phase
    size_t skipSamples = 0;      // output samples thrown away first: starts the stream inside a PPDU
    std::vector<HalowGenFrame> frames;
};

class HalowGenerator {
public:
    explicit HalowGenerator(const HalowGenConfig& c);
    ~HalowGenerator();
    void generate(cf32* out, size_t n);
    static double noiseSigma(const HalowGenConfig& c);   // per real component
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

double halowFrameSeconds(const HalowGenFrame& f);        // duration of the PPDU on the air (without the gap)

// The networks of the schedule: a 1 MHz WPA3 network, a 2 MHz WPA2 one, a 4 MHz open one (US plan), one that first announces only its compressed
// SSID and answers a probe later, a probe response, a data frame, one PPDU with two spatial streams (only its SIG field is read). At a radio rate of
// 10 Msps or more an 8 MHz network joins, at 20 Msps a 16 MHz one.
std::vector<HalowGenFrame> halowTestSchedule(double radioRate = 4e6);

std::unique_ptr<ModeSynth> makeHalowSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
