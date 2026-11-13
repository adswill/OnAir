// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LBJ link layer: POCSAG codewords (BCH(31,21) + even parity) turned into numeric pages. Original implementation from the POCSAG
// standard (ITU-R M.584); no third-party code.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::lbj {

constexpr uint32_t kSyncWord = 0x7CD215D8u;
constexpr uint32_t kIdleWord = 0x7A89C197u;

// A burst as the modem hands it over: the codewords that followed each batch sync (16 per batch), polarity already corrected.
struct Burst {
    double timeSec = 0;
    float levelDb = -200;
    float cfoHz = 0;
    int syncErrors = 0;                  // bit errors in the first sync word
    std::vector<uint32_t> words;         // 16 per batch
    std::vector<float> rel;              // optional: per bit (32 per word) how far the soft value was from the slicer threshold, 1.0 = typical
};

struct CwResult {
    bool ok = false;
    uint32_t word = 0;                   // corrected word
    int fixed = 0;                       // bits corrected (0 to 2)
};

uint32_t bchRemainder(uint32_t word);    // remainder of the top 31 bits by g(x) = x^10+x^9+x^8+x^6+x^5+x^3+1
bool codewordValid(uint32_t word);       // remainder zero and even parity
CwResult correctCodeword(uint32_t word, int maxFix = 2);
// With the per-bit reliabilities: tries flipping the least reliable bits first (Chase-style), then the BCH correction; picks the closest word.
CwResult correctSoft(uint32_t word, const float* rel32);
uint32_t encodeCodeword(uint32_t data21); // the 21 information bits in the low bits -> the full 32-bit word

// Encoders for the generator and the tests.
uint32_t addressWord(uint32_t ric, int function, int* slot = nullptr);   // slot (frame 0..7) is ric & 7
uint32_t messageWord(uint32_t data20);

// A page: the address codeword and the message codewords that followed it in the same batch run.
struct Page {
    uint32_t ric = 0;
    int function = 0;
    std::string digits;                  // numeric text, the 4-bit codes mapped with the POCSAG table: 0-9, '*' (spare), 'U', ' ', '-', ')' and '('
    int fixed = 0;                       // bits corrected in the address and the first three message codewords
    int missing = 0;                     // message codewords lost to uncorrectable errors (their five digits are '?')
    bool addrFixedTwo = false;
};

std::string numericDigits(uint32_t data20);                // five characters
std::vector<Page> pagesFromBurst(const Burst& b);

} // namespace dect2::lbj
