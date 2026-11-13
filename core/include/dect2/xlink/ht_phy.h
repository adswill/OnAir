// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// IEEE 802.11-2020 clause 19 (HT, "802.11n") mixed-format PHY, one spatial stream, 20 MHz, BCC: what wfb-ng and OpenHD transmit. The existing Wi-Fi
// receiver (core/src/wifi) decodes non-HT frames only; this one reads the L-SIG and HT-SIG fields and decodes MCS 0 - 7, long or short guard interval.
//
// Not decoded (counted as unsupported, never guessed): two or more streams (MCS 8 and up), STBC, LDPC, 40 MHz, greenfield format, A-MPDU aggregates.
// Everything is at 20 Msps. Receive only; htPpdu() builds the test signal in memory.
//
//   L-STF 8 us, L-LTF 8 us, L-SIG 4 us (6 Mbit/s, length field spoofed), HT-SIG 8 us (two symbols, BPSK on the Q axis), HT-STF 4 us, HT-LTF 4 us, data symbols
//   with 56 used carriers (52 data, 4 pilots at -21, -7, 7, 21), 4 us or 3.6 us each.
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace dect2::xlink::ht {

constexpr double kRate = 20e6;

struct Mcs { int bpsc, dbps; int punct; };             // punct: 0 = 1/2, 1 = 3/4, 2 = 2/3, 3 = 5/6
Mcs mcsParams(int mcs);                                 // mcs 0 .. 7; table 19-27

// HT-SIG CRC: the 8 bits of Figure 19-6 over bits 0 .. 33, transmitted c7 first (value here has c7 as the top bit)
uint8_t htSigCrc(const uint8_t* bits34);

// A whole HT-mixed PPDU at 20 Msps. mpdu includes its FCS. Unit carriers: unit average power in the data part.
std::vector<cf32> htPpdu(const std::vector<uint8_t>& mpdu, int mcs, bool shortGi = false, uint8_t scrSeed = 0x5D);

struct HtInfo {
    int mcs = 0;
    bool shortGi = false;
    int length = 0;          // PSDU octets, FCS included
    double cfoHz = 0;
    float snrDb = 0, rssiDb = -200;
    int64_t at = 0;          // sample index of the L-LTF
};

class HtRx {
public:
    using FrameCb = std::function<void(const uint8_t* mpdu, size_t nNoFcs, const HtInfo&)>;
    HtRx();
    ~HtRx();
    void setCallback(FrameCb cb);
    void reset();
    void feed(const cf32* x, size_t n);                 // 20 Msps, any block size
    struct Stats { uint64_t triggers = 0, noSync = 0, nonHt = 0, sigBad = 0, unsupported = 0, framesOk = 0, framesBad = 0; };
    const Stats& stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2::xlink::ht
