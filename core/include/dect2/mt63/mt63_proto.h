// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MT63 (Pawel Jalocha SP9VRC) coding layer: the pulse shape, Walsh-Hadamard character code, interleaver and the differential bit rule.
//
// 64 carriers, each sent as differential BPSK (bit 1 = no phase change, bit 0 = reversal) once per symbol, 10 symbols/s in the 1000 Hz
// mode (5 in 500 Hz, 20 in 2000 Hz). One 7-bit character per symbol: the character is a Walsh-Hadamard codeword over the 64 carriers
// (code 0..63 = row `code`, 64..127 = the same row inverted), the 64 bits of one character are spread over the next 32 (short) or
// 64 (long) symbols by a fixed per-carrier delay (the interleaver). The pulse shape (512 samples at the internal rate, symbols 200
// samples apart, carriers 4 FFT bins apart) is the one of the MT63 modem as published with its sources.
// Parameters, the pulse table and the interleaver step pattern: fldigi (GPL-3.0-or-later, from SP9VRC's MT63): credits row in tools/credits_data.tsv.
#pragma once
#include <cstdint>
#include <vector>

namespace dect2::mt63 {

constexpr int kCarriers = 64;
constexpr int kFft = 512;            // pulse length and FFT size, in samples of the internal rate
constexpr int kSymSep = 200;         // samples between symbols
constexpr int kCarrSep = 4;          // FFT bins between carriers
constexpr int kShortIntl = 32, kLongIntl = 64;

inline bool validBandwidth(int bw) { return bw == 500 || bw == 1000 || bw == 2000; }
inline double internalRate(int bw) { return 2.0 * bw; }                 // complex samples per second of the pulse/FFT domain
inline double symbolSec(int bw) { return kSymSep / internalRate(bw); }  // 0.2 s, 0.1 s, 0.05 s
inline double carrierSpacingHz(int bw) { return bw / 64.0; }
inline double charsPerSec(int bw) { return 1.0 / symbolSec(bw); }
inline int intlLen(bool longIntl) { return longIntl ? kLongIntl : kShortIntl; }

// The pulse: p(n) for n in [0, 512], linear-in-cubic (Catmull-Rom) interpolation of the published table; 0 outside.
double pulse(double n);

// Delay (in symbols) of carrier i's bit of a character: the bits of character c go out on carrier i in symbol c + delay(i)
int intlDelay(bool longIntl, int carrier);

// Walsh-Hadamard code of a 7-bit character: w[i] = +1 / -1 on carrier i
void walshEncode(int code, int8_t w[kCarriers]);
// Soft vector s[i] (> 0: the bit is 0, i.e. w = +1) -> code; conf = the peak over the rms of the other 63 correlations
int walshDecode(const float s[kCarriers], float& conf);

// Transmit side: one character in, the 64 bits (1 = keep the phase, 0 = reverse it) of this symbol out
class Encoder {
public:
    explicit Encoder(bool longIntl, uint32_t seed = 1);
    void process(int code, uint8_t bits[kCarriers]);
private:
    bool longIntl_;
    int ptr_ = 0;
    std::vector<uint8_t> pipe_;   // [symbol][carrier] ring: the Walsh bits (1 = negative) of the last intlLen characters
    uint32_t rng_;
};

// Receive side: soft bit vectors in (u[i] > 0 means "the phase reversed", i.e. Walsh bit 0), characters out once the pipe is full
class Deinterleaver {
public:
    explicit Deinterleaver(bool longIntl);
    // Adds the symbol's soft values; returns true when a character is complete: code (0..127) and the peak quality
    bool push(const float u[kCarriers], int& code, float& conf);
    void reset();
    int length() const { return len_; }
private:
    int len_;
    bool longIntl_;
    std::vector<float> ring_;   // [len][carrier]
    int pos_ = 0, filled_ = 0;
    int delay_[kCarriers];
};

} // namespace dect2::mt63
