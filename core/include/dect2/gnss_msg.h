// The coded navigation messages: SBAS L1 (RTCA DO-229: 250 bit messages at 250 bps, rate 1/2 convolutional code, 500 symbols a second) and
// Galileo I/NAV on E1-B (Galileo OS SIS ICD sections 4.1.4, 4.3 and 5.1: page parts of 120 bits, convolutional code with the second branch inverted,
// a 30 x 8 block interleaver, a 10 symbol synchronisation pattern, 250 symbols a second; words of 128 bits protected by a CRC over the even and odd parts).
// Both codes: constraint length 7, G1 = 171 octal, G2 = 133 octal, G1's symbol first. Both CRCs: the 24 bit CRC of G(X) = (1 + X) P(X) (CRC-24Q).
#pragma once
#include "gnss_nav.h"
#include <cstdint>
#include <vector>

namespace dect2 {

// ------------------------------------------------------------------ shared layers
// Encode nBits bits from encoder state `state` (the last six input bits, the newest in bit 5); out gets 2 * nBits symbols (0/1). Returns the end state.
int convEncode(const uint8_t* bits, int nBits, bool invertG2, int state, uint8_t* out);
// Maximum likelihood decoding of 2 * nBits soft symbols (positive = logic 0, the larger the surer). startState / endState -1: unknown.
void viterbiDecode(const float* soft, int nBits, bool invertG2, int startState, int endState, uint8_t* out);
// CRC-24Q of bits[0..n) (one bit per byte, the first is the most significant)
uint32_t crc24q(const uint8_t* bits, int n);

// ------------------------------------------------------------------ SBAS
constexpr int kSbasMsgBits = 250;
constexpr uint8_t kSbasPreamble[3] = {0x53, 0x9A, 0xC6};      // the 24 bit preamble, one byte per message in turn
struct SbasMessage {
    int64_t bitPos = 0;          // index of its first bit in the decoded stream
    int type = 0;                // 0..63
    uint8_t bits[kSbasMsgBits];
};
// The message bits with the preamble byte of message number `index` (index % 3), the type, 212 data bits and the CRC
void sbasBuildMessage(int index, int type, const uint8_t* data212, uint8_t* out250);
// Decode a window of soft symbols (aligned to the bit pairs, unknown state at both ends) and find the messages with a good CRC in it.
// The first and last `margin` bits are not trusted (the decoder has no state there). Either polarity of the symbols is accepted.
std::vector<SbasMessage> sbasFindMessages(const float* soft, int nBits, int margin = 24);

// ------------------------------------------------------------------ Galileo I/NAV
constexpr int kInavPartSymbols = 250;                          // 10 sync + 240 coded
constexpr uint8_t kInavSync[10] = {0, 1, 0, 1, 1, 0, 0, 0, 0, 0};
void inavInterleave(const uint8_t* in240, uint8_t* out240);   // written into 30 columns of 8, read out by the 8 rows (ICD Table 25, checked against Annex D.2)
void inavDeinterleave(const float* in240, float* out240);
// 120 bits of a page part (the last six are the zero tail) to its 250 symbols (sync pattern first)
void inavEncodePart(const uint8_t* bits120, uint8_t* out250);
// 240 soft symbols after the sync pattern to the 120 bits of the part
void inavDecodePart(const float* soft240, uint8_t* bits120);
// The two parts of a nominal E1-B page from a 128 bit word (even first): OSNMA, SAR and spare are given (40 + 22 + 2 bits, may be zero), ssp 0..2 = SSP1..3
void inavBuildPage(const uint8_t* word128, const uint8_t* osnmaSarSpare64, int ssp, uint8_t* even120, uint8_t* odd120);
// Check a page: the even and the odd part (120 bits each); on success the 128 bit word is put in word128
bool inavCheckPage(const uint8_t* even120, const uint8_t* odd120, uint8_t* word128);

// Fields of a 128 bit word (bit 0 is the first of the word type)
uint32_t inavField(const uint8_t* w, int first, int len);
int32_t inavFieldSigned(const uint8_t* w, int first, int len);
void inavPut(uint8_t* w, int first, int len, uint32_t v);

// What the I/NAV words of one satellite give (words 1-6 and 10)
struct GalNav {
    int iod[5] = {-1, -1, -1, -1, -1};                         // IODnav of words 1..4 as received
    GpsEphemeris eph;                                          // being collected (galileo = true)
    int svid = 0;
    bool w5 = false;
    double ai[3] = {0, 0, 0};
    double bgdE1E5a = 0, bgdE1E5b = 0;
    int e1bHs = 0, e1bDvs = 0;
    int wn = -1;                                               // GST week (12 bits)
    bool ggtoValid = false;
    double a0g = 0, a1g = 0, t0g = 0;
    int wn0g = 0;
    bool complete() const { return iod[1] >= 0 && iod[1] == iod[2] && iod[1] == iod[3] && iod[1] == iod[4] && w5; }
};
// Fold a word into the collection; returns the word type. A TOW (word types 0 with Time = 2, 5, 6) is put in *tow (else -1).
int inavParseWord(const uint8_t* w, GalNav& nav, int* tow);

} // namespace dect2
