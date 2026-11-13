// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MPT 1327 control channel test signal: codewords from the standard's field layouts, a control channel bit stream
// (start-up preamble, SYNC, then slots of a control channel system codeword and an address codeword) and its FFSK audio / FM signal.
#pragma once
#include "dect2/mpt1327/mpt1327_proto.h"
#include <complex>
#include <cstdint>
#include <vector>

namespace dect2::mpt1327 {

uint64_t makeCcsc(int sys);                                                  // control channel system codeword (final 16 bits = SYNC)
uint64_t makeGtc(int pfix, int ident1, int d, int chan, int ident2, int n = 0);
uint64_t makeAloha(int pfix, int ident1, int func, int chan4, int wt = 0, int m = 0, int n = 0);
uint64_t makeAck(int pfix, int ident1, int func, int ident2, int qual = 0, int n = 0);
uint64_t makeAhy(int pfix, int ident1, int func = 0, int params18 = 0);
uint64_t makeBcast(int sysdef, int sys, int params18);                      // params: the 18 bits after FUNC
uint64_t makeBcastChannel(int sysdef, int sys, int chan, int adjsite = 0);  // announce (0), withdraw (1) or adjacent site (4) control channel

// A data codeword with the given 47 bits of information (bit 1 = 0).
uint64_t makeData(uint64_t info47);

// Bits (one per element, 0 / 1) of a control channel transmission: preamble of 'preamble' bit reversals ending in 0, SYNC, the first
// address codeword, then a CCSC and an address codeword for every further one. 'lead' extra reversal bits before it.
std::vector<uint8_t> controlChannelBits(int sys, const std::vector<uint64_t>& addressCodewords, int preamble = 16);

// FFSK audio at fs Hz for the bits: bit 1 -> 1200 Hz and bit 0 -> 1800 Hz with continuous phase (or the opposite when invert is set).
std::vector<float> ffskAudio(const std::vector<uint8_t>& bits, double fs, bool invert = false, float amp = 0.5f, double bitRateScale = 1.0);

// Frequency modulation of audio: complex baseband at fs with the carrier at offsetHz, peak deviation devHz for an audio value of 1.
std::vector<std::complex<float>> fmModulate(const std::vector<float>& audio, double fs, double devHz, double offsetHz);

}  // namespace dect2::mpt1327
