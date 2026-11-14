// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Robust Packet Radio (RPR, SCS): demodulation of the physical layer (receive only), see docs/rpr.md.
//
// Confirmed by the public SCS / Wavecom descriptions: eight-carrier pulse-shaped OFDM, audio centre 1500 Hz, carrier centre spacing 60 Hz (500 Hz
// bandwidth), every carrier DBPSK (speed level 1, convolutional code R = 1/2) or DQPSK (speed level 2, R = 3/4) at a constant 50 baud, a 160 ms preamble of
// 64 DPSK symbols over the eight carriers, constraint length 9. NOT public: the generator polynomials, the puncturing, the scrambler, the full-frame
// interleaver, the callsign compression, the 25 packet types and the CRC. So this module finds the bursts, the comb, the symbol clock and the speed
// level and delivers the differential symbols; it does not produce AX.25 frames (nothing is invented).
//
// All functions work on 8 kHz real audio (the upper sideband, as the other HF modes do).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 { namespace rpr {

constexpr double kFs = 8000;
constexpr double kBaud = 50;
constexpr double kSpacingHz = 60;
constexpr int kCarriers = 8;
constexpr double kNominalCentreHz = 1500;

struct Burst {
    double startSec = 0, lenSec = 0;     // start within the analysed audio, duration
    int order = 0;                       // 2 (D-BPSK, speed level 1), 4 (D-QPSK, speed level 2), 0 not decided
    int level = 0;                       // speed level 1 or 2
    double snrDb = 0;                    // from the spread of the phase steps (EVM), in the 50 baud symbol bandwidth
    double r = 0;                        // concentration of the phase steps (1 = perfect)
    int carriers = 0;                    // carriers whose phase steps are concentrated (r >= 0.3): the comb is really there
    int nSyms = 0;                       // symbols per carrier
    double clockPpm = 0;
    std::vector<std::vector<uint8_t>> sym;   // [carrier 0..7 low to high frequency][symbol]: phase step index 0 .. order-1
};

struct Analysis {
    bool found = false;
    double centreHz = 0;                 // centre of the comb
    double combDb = 0;                   // clock line strength of the carriers (mean, dB over the noise)
    int carriersSeen = 0;                // carriers with a 50 baud line
    double snrDb = 0;                    // of the strongest burst
    std::vector<double> carrierHz;
    std::vector<Burst> bursts;           // oldest first
    std::string note;
};

Analysis analyse(const float* x, size_t n);

// Carrier frequencies for a comb centre
std::vector<double> carriersFor(double centreHz);
// bits per symbol
inline int bitsOf(int order) { return order <= 2 ? 1 : 2; }

}} // namespace dect2::rpr
