// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Kris modes (KAVB, KAPP, KAFT, KAAL): QAM mapping and soft demapping, CRCs, Zadoff-Chu sequences, root-raised-cosine taps, the xorshift64 generator. Pure C++.
#pragma once
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2::kris {

enum class Mod : int { Bpsk = 1, Qpsk = 2, Qam16 = 4, Qam64 = 6, Qam256 = 8, Qam1024 = 10 };   // value = bits per symbol

// Gray mapped, unit average power. The bits of a symbol are taken first-transmitted first: the first half label the I axis, the second half the Q axis; in each
// half the first bit is the sign (0 = positive) and the rest follow a Gray code, so neighbours differ in one bit. BPSK: bit 0 -> +1, bit 1 -> -1 (real axis).
// A last partial symbol is padded with zero bits. nBits bits give ceil(nBits / bitsPerSymbol) symbols.
std::vector<std::complex<float>> qamMap(const uint8_t* bits, size_t nBits, Mod m);
// Max-log LLRs, n x bitsPerSymbol outputs in the order of qamMap; positive = bit 0 more likely. noiseVar = total complex noise variance per symbol (N0 when Es = 1).
void qamDemapSoft(const std::complex<float>* sym, size_t n, Mod m, float noiseVar, float* llr);

uint16_t crc16(const uint8_t* d, size_t n);   // CRC-16-CCITT, polynomial 0x1021, init 0xFFFF, no reflection ("123456789" -> 0x29B1)
uint32_t crc32(const uint8_t* d, size_t n);   // zlib CRC-32 ("123456789" -> 0xCBF43926)

// Zadoff-Chu sequence x[n] = exp(-j pi root n (n + 1) / length) (odd length), exp(-j pi root n^2 / length) (even length); unit magnitude.
std::vector<std::complex<float>> zadoffChu(int root, int length);
// Root-raised-cosine pulse, spanSymbols * sps + 1 taps centred on the middle tap, unit energy (sum of squares = 1).
std::vector<float> rrcTaps(float beta, int sps, int spanSymbols);

struct XorShift64 {   // shifts 13, 7, 17 (the same generator as the Rust code tables)
    uint64_t s;
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
};

}  // namespace dect2::kris
