// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cell towers: the channel coding of the broadcast channels (3GPP TS 36.212), written from the specification.
//
// Bits are uint8_t 0/1. Soft values are log-likelihood ratios, positive = bit 0 more likely. NULL (dummy or filler) bits in the rate
// matching are marked internally and never transmitted.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2::cell {

using Bits = std::vector<uint8_t>;

// CRCs (36.212 5.1.1), MSB first: the parity of a over len bits
uint32_t crc(const uint8_t* a, size_t n, uint32_t poly, int len);
constexpr uint32_t kCrc24A = 0x864CFB, kCrc24B = 0x800063, kCrc16 = 0x1021, kCrc8 = 0x9B, kCrc24C = 0xB2B117, kCrc11 = 0x621, kCrc6 = 0x21;
void appendCrc(Bits& b, uint32_t poly, int len, uint32_t xorMask = 0);   // append the parity, its last bits xor-ed with the mask
bool checkCrc(const Bits& b, uint32_t poly, int len, uint32_t xorMask = 0);   // over the whole vector (payload + parity)

// Gold sequence c(n) of 36.211 7.2 / 38.211 5.2.1, n = 0 .. len-1
void gold(uint32_t cinit, uint8_t* c, size_t len);
Bits gold(uint32_t cinit, size_t len);

// Tail-biting convolutional code, rate 1/3, constraint length 7, G = 133, 171, 165 (octal): out is d0[0..K-1], d1[...], d2[...]
Bits convEncode(const Bits& in);
// Its rate matching (5.1.4.2) to E bits, and the inverse that adds the soft values of the E received bits into 3*K soft values
Bits convRateMatch(const Bits& d012, size_t E);
std::vector<float> convRateRecover(const float* e, size_t E, size_t K);
// Wrap-around Viterbi decoder of the tail-biting code; llr holds d0, d1, d2 of K bits each. Returns the K bits.
Bits viterbiTb(const float* llr, size_t K);

// Turbo code (5.1.3.2): K must be one of the 188 interleaver sizes. F filler bits at the start are taken as 0 (and not sent).
bool turboSizeOk(int K);
int turboSizeFor(int B);        // smallest K >= B (B <= 6144), 0 when none
void qppParams(int K, int& f1, int& f2);
// out: d0, d1, d2 of K + 4 bits each (the 12 tail bits placed as in 5.1.3.2.2)
Bits turboEncode(const Bits& in);
// Rate matching (5.1.4.1) of one code block of K+4 bit streams to E bits with redundancy version rv; F filler bits; ncb 0 = the whole buffer
Bits turboRateMatch(const Bits& d012, int K, int F, size_t E, int rv);
// The inverse: adds the soft values of the E received bits into d0, d1, d2 (3 * (K+4) values)
void turboRateRecover(const float* e, size_t E, int K, int F, int rv, std::vector<float>& d012);
// Max-log-MAP turbo decoder: d012 as from turboRateRecover; F filler bits are known zeros. The decoded K bits go to out; the check
// function (when given) is called after each iteration on the hard decisions and stops the iterations when it returns true.
bool turboDecode(const std::vector<float>& d012, int K, int F, int iterations, Bits& out, bool (*check)(const Bits&, void*) = nullptr, void* ctx = nullptr);

// The 32-column sub-block interleaver's column permutations: turbo code (Table 5.1.4-1) and convolutional code (Table 5.1.4-2, also
// the PDCCH quadruplet interleaver of 36.211 6.8.5)
extern const int kSubblockPerm[32];
extern const int kSubblockPermConv[32];

} // namespace dect2::cell
