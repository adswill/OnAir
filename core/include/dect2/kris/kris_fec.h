// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Kris modes (KAVB, KAPP, KAFT, KAAL): the channel codes, a thin wrapper over the Rust crate rust/onair_kris (C ABI core/include/dect2/rs/kris.h).
// LDPC family: n = 1920, 2304, 7680; rates 1/4 .. 9/10 (n = 2304 has no 9/10: ldpcK gives -1). Bits are one per byte. LLR: positive = bit 0 more likely.
#pragma once
#include <cstdint>
#include <vector>

namespace dect2::kris {

enum class LdpcRate : int { R1_4 = 0, R1_3 = 1, R1_2 = 2, R2_3 = 3, R3_4 = 4, R5_6 = 5, R9_10 = 6 };

// Information bits of the code, or -1 when (n, rate) is not a supported code.
int ldpcK(int n, LdpcRate r);
// info = k bits, codeword = n bits (the information bits first, then the parity). False for an unsupported code.
bool ldpcEncode(int n, LdpcRate r, const uint8_t* info, uint8_t* codeword);
// llr = n values, infoBits = k bits (written also on failure). Returns the iterations used (0 = already a codeword) or -1 when the checks never passed.
int ldpcDecode(int n, LdpcRate r, const float* llr, uint8_t* infoBits, int maxIter = 50);

// K = 7 convolutional code. kind 0 = rate 1/2 (0171, 0133), 1 = rate 1/4 (0171, 0133, 0165, 0117). terminate appends six zero tail bits. Empty on a bad kind.
std::vector<uint8_t> convEncode(int kind, const std::vector<uint8_t>& bits, bool terminate);
// llr holds groups of 2 (kind 0) or 4 (kind 1) values; terminated drops the six tail bits. Empty on a bad kind or a block shorter than the tail.
std::vector<uint8_t> viterbiDecode(int kind, const std::vector<float>& llr, bool terminated);

}  // namespace dect2::kris
