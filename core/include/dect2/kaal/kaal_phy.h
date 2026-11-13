// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL physical layer constants and helpers (port of the Python prototype kaal_phy.py): 16-QAM, interleaver, sweep, RRC.
#pragma once
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 {
namespace kaal {

constexpr int kFs = 48000;                 // baseband sample rate
constexpr int kSym = 8000;                 // symbols per second
constexpr int kSps = kFs / kSym;           // 6
constexpr double kBeta = 0.25;             // root-raised-cosine roll-off
constexpr int kRrcSpan = 8;                // symbols
constexpr int kPilotEvery = 16;            // every 16th symbol is a pilot
constexpr int kPreambleLen = 32;           // known symbols at the frame start
constexpr int kFrameInfoBits = 320;
constexpr int kCodedBits = 640;
constexpr int kQamSymbols = kCodedBits / 4;                  // 160
constexpr int kNPilots = kQamSymbols / (kPilotEvery - 1);    // 10
constexpr int kFrameSymbols = kPreambleLen + kQamSymbols + kNPilots;
// v3 voice frames of 160 symbols = 20 ms at 8000 sym/s: 32 preamble + 120 data + 8 pilots, 480 coded bits per frame (see kaal_fec.h,
// ldpcEncodeRm). The block interleaver spans 4 frames (1920 bits).
constexpr int kRtCodedBits = 480;
constexpr int kRtQamSymbols = kRtCodedBits / 4;                       // 120
constexpr int kRtNPilots = kRtQamSymbols / (kPilotEvery - 1);         // 8
constexpr int kRtFrameSymbols = kPreambleLen + kRtQamSymbols + kRtNPilots;   // 160
constexpr int kRtBlockFrames = 4;
constexpr double kSweepPeriodS = 0.5;
constexpr double kSweepLowHz = -5000.0;
constexpr double kSweepHighHz = 5000.0;
constexpr double kSweepPowerDb = -10.0;
constexpr uint64_t kInterleaveSeed = 0x4B41414CULL;   // "KAAL"

using cf = std::complex<float>;

cf pilotSymbol();                                  // (3+3j)/sqrt(10)
std::vector<cf> preamble();                        // 32 known symbols, unit average power

// 4 bits per symbol, order I0 I1 Q0 Q1, Gray levels (00 -3, 01 -1, 11 +1, 10 +3), unit average power.
std::vector<cf> qam16Map(const uint8_t* bits, size_t nbits);
// Max-log soft bits, positive = bit 0 more likely. noiseVar is per symbol on the unit-power scale; 4 LLRs per symbol.
std::vector<float> qam16DemapSoft(const cf* sym, size_t n, float noiseVar);

// xorshift64 (13,7,17), the portable generator shared with the Python prototype.
uint64_t xorshift64(uint64_t& state);
// out[i] = in[perm[i]]; Fisher-Yates from the top, j = draw % (i+1), seeded with kInterleaveSeed.
std::vector<uint32_t> interleavePerm(size_t n);
std::vector<uint32_t> interleavePermInv(size_t n);
std::vector<uint8_t> interleave(const std::vector<uint8_t>& in);
std::vector<float> deinterleave(const std::vector<float>& in);   // soft values: out[perm[i]] = in[i]

std::vector<double> rrcTaps();                     // unit-energy, length kRrcSpan*kSps+1
std::vector<cf> sweepTemplate(size_t nSamples, double t0 = 0.0);

}  // namespace kaal
}  // namespace dect2
