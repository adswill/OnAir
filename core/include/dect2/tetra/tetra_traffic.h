// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRA speech traffic channel (TCH/S) channel coding, written from the public ETSI EN 300 395-2 V1.3.1 (2005-01), clause 5.5 (normal speech
// traffic channel, two speech frames per time slot) and the table 4 / table 5 bit orders. CHANNEL coding only: the 137 bit speech frames that come
// out are handed to the user-installed speech codec plug-in (codec/speech_codec.h); no speech codec is part of OnAir.
//
// Receive chain (the reverse of clause 5): type-5 bits -> descrambling with the cell's colour code (tetra_voice.cpp) -> type-4 bits ->
// block deinterleaving (5.5.3, transposition of the (24,18) matrix) -> type-3 bits -> depuncturing (5.4.3.2, rate 2/3 for class 1, 8/18 for class 2)
// and Viterbi decoding of the 16-state rate 1/3 mother code (5.4.3.1) -> type-2 bits -> CRC check of the class 2 bits (5.5.1) and re-ordering by
// table 5 -> the two speech frames (type-1 bits, numbered B1..B137 as in table 4).
//
// Frame format handed to the plug-in: one frame is 137 bytes, one bit per byte (0 or 1), byte i = speech frame bit B(i+1) of table 4, i.e. the
// order of the source encoder output (clause 4.2.2.7). decodeTchS() returns both frames of the burst, first frame (A) then second (B), 274 bytes.
//
// Not covered (left out on purpose): the stolen-frame half slot mode (5.6, signalling in the first half slot: needs the STCH interleaver of
// EN 300 392-2 clause 8 and the stealing flag from the training sequence, not decoded here), and the optional AMR codec (clauses 9 - 12).
#pragma once
#include "dect2/tetra/tetra_voice_codec.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2::tetra {

constexpr int kTchSFrameBits = 137;        // bits of one speech frame (30 ms)
constexpr int kTchSBurstBits = 432;        // type-3 / type-4 bits of a full slot traffic burst
constexpr int kTchSType2Bits = 286;        // type-2 bits of the two frames: 2 x (51 + 56 + 30) + 8 parity + 4 tail

// Decodes the 432 DESCRAMBLED type-4 bits (one bit per byte) of a normal speech traffic burst. Returns the two speech frames, 2 x 137 bytes,
// or an empty vector when the burst is not 432 bits or the CRC over the class 2 bits fails (the BFI of clause 6.2: both frames are dropped).
// *crcOk (optional) tells which of the two.
std::vector<uint8_t> decodeTchS(const uint8_t* type4, size_t n, bool* crcOk = nullptr);

// The transmit side of clause 5.5 (used by the tests and the signal generator): two 137 bit speech frames -> 432 type-4 bits (before scrambling).
void encodeTchS(const uint8_t* frameA, const uint8_t* frameB, uint8_t* type4);

// Table 5 of the standard: the speech frame bit number (B1..B137) behind each of the 137 pairs of type-2 bits (frame A, frame B); for the tests.
const uint8_t* tchSBitOrder();

// The decoder for TetraVoiceSink::setFrameDecoder(); it never looks at encryption (that gate is in processTrafficBurst, before any burst bit is read).
TrafficFrameDecoder makeTrafficFrameDecoder();

// Bursts decoded / dropped by the CRC since start (all decoders together), for the status text
uint64_t tchSBurstsOk();
uint64_t tchSBurstsCrcFailed();

} // namespace dect2::tetra
