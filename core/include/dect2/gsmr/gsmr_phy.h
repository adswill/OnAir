// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GSM-R (GSM 900 downlink): the physical layer of the broadcast control channel, written from 3GPP TS 45.001 / 45.002 / 45.003 / 45.004.
//
// Only the downlink broadcast is handled: the frequency correction burst (FCCH), the synchronisation burst (SCH, BSIC and frame number)
// and the BCCH normal bursts that carry System Information. Nothing addressed to a mobile (paging, assignments, traffic, the uplink) is read.
//
// Bits are uint8_t 0/1. Soft values are log-likelihood ratios, positive = bit 0 more likely. Sample streams are complex baseband at four
// samples per symbol (1083.333 kHz) unless stated.
#pragma once
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2::gsmr {

using Bits = std::vector<uint8_t>;

constexpr double kSymbolRate = 1625000.0 / 6.0;   // 270.8333 kHz
constexpr int kSps = 4;                            // samples per symbol of the receiver's channel stream
constexpr double kStreamRate = kSymbolRate * kSps;
constexpr int kBurstSyms = 148;
constexpr int kFrameSamples = 5000;                // one TDMA frame (8 slots, 1250 symbols) at kSps

extern const uint8_t kSync[64];      // the SCH extended training sequence
extern const uint8_t kTsc[8][26];    // the normal burst training sequences
extern const uint8_t kDummy[148];    // the dummy burst

// ---------------------------------------------------------------- channel coding
// The 40 FIRE-code parity bits (as transmitted, inverted) of 184 data bits; the SCH's 10 parity bits of its 25
Bits fireParity(const uint8_t* d184);
Bits schParity(const uint8_t* d25);
// Rate 1/2 constraint length 5 convolutional code (G0 = 1 + D^3 + D^4, G1 = 1 + D + D^3 + D^4), encoder starting in state 0
Bits convEncode(const Bits& u);
// Soft Viterbi decoder; llr holds 2 * nu values, the encoder is taken to end in state 0. Returns the nu bits.
Bits convDecode(const float* llr, size_t nu);

// A 23 byte layer 2 frame (BCCH, CCCH, SACCH) -> 456 coded bits -> four bursts of 114 bits, and back
Bits xcchEncode(const uint8_t* msg23);
void xcchInterleave(const Bits& coded456, Bits out[4]);
void xcchDeinterleave(const float* burst114[4], float* llr456);
bool xcchDecode(const float* llr456, uint8_t* msg23);   // false when the FIRE code check fails
// The SCH: BSIC (6 bits) and the frame number -> 78 coded bits, and back
Bits schEncode(int bsic, uint32_t fn);
bool schDecode(const float* llr78, int& bsic, uint32_t& fn);
inline uint32_t frameFromSch(int t1, int t2, int t3) { return (uint32_t)(51 * (((t3 - t2) % 26 + 26) % 26) + t3 + 51 * 26 * t1); }

// ---------------------------------------------------------------- bursts (148 bits each)
Bits normalBurst(const Bits& data114, int tsc, int stealing = 0);   // stealing: the two flag bits (1 for the control channels BCCH / CCCH)
Bits syncBurst(const Bits& coded78);
Bits fcchBurst();

// ---------------------------------------------------------------- modulation (the test signal)
// GMSK with BT = 0.3, differential encoding as in 45.004. Output at sps samples per symbol over 152 symbols: the first sample is 2 symbols
// before the burst's first symbol. Amplitude 1 with raised-cosine ramps outside the burst.
std::vector<cf32> gmskBurst(const Bits& burst148, int sps);

// ---------------------------------------------------------------- reception
struct FcchHit { double center = 0; double freqErrHz = 0; float coherence = 0; };   // centre in samples of the stream
std::vector<FcchHit> findFcch(const cf32* x, size_t n);

struct BurstRx {
    bool ok = false;
    double pos = 0;               // sample of the burst's first symbol (in the stream)
    float snrDb = -99;
    float llr[148] = {};
};
// Find the burst of a known training sequence near the predicted position (searchSyms symbols either side), estimate the channel on the
// training sequence and equalise (maximum-likelihood soft output).
BurstRx demodBurst(const cf32* x, size_t n, double predicted, int searchSyms, const uint8_t* train, int trainLen, int trainPos);

// Down-convert and decimate a stretch of the radio's samples to one 200 kHz channel at kStreamRate: x[start, start + count) mixed down by
// shiftHz (relative to fs). Returns the samples of the stream.
std::vector<cf32> channelize(const cf32* x, size_t n, double fs, double shiftHz, size_t start, size_t count);

} // namespace dect2::gsmr
