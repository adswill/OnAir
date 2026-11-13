// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRA downlink physical layer and lower MAC (ETSI EN 300 392-2 clauses 8 and 9): pi/4-DQPSK at 18 ksym/s, the continuous downlink
// bursts (normal and synchronisation), the training sequences, and the channel coding of the logical channels a base station broadcasts
// to every terminal: CRC-16, the 16-state rate 1/4 mother code punctured to 2/3 (RCPC), block interleaving, the colour code scrambling,
// and the Reed-Muller (30,14) code of the access assignment channel (AACH).
// Receive side only works on the broadcast and common control channels; traffic (voice) bursts are never decoded.
#pragma once
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2::tetra {

using Bits = std::vector<uint8_t>;   // one bit per byte, in transmission order

constexpr double kSymRate = 18000.0;
constexpr int kSps = 4;                         // the receiver and the generator work at 4 samples per symbol
constexpr double kRate = kSymRate * kSps;       // 72 kHz
constexpr int kSlotSyms = 255, kSlotBits = 510; // one timeslot of the continuous downlink: 510 modulation bits (14.167 ms)
constexpr int kSlotSamples = kSlotSyms * kSps;
constexpr double kRolloff = 0.35;

// Training sequences (9.4.4.3): normal 1 (n), normal 2 (p), normal 3 (q, split over the slot boundary), synchronisation (y); the frequency
// correction field of the synchronisation burst (f)
extern const uint8_t kTrainN[22], kTrainP[22], kTrainQ[22], kTrainY[38], kFreqCorr[80];

// The phase step of a dibit (b1, b2): 00 +pi/4, 01 +3pi/4, 11 -3pi/4, 10 -pi/4 (9.3.3), as a multiple of pi/4
int dibitPhase(int b1, int b2);

// TEDS pi/8-D8PSK (clause 5.4, table 5.2): the step of three bits as a multiple of pi/8: 000 +1, 001 +3, 101 +5, 100 +7, 010 -1, 011 -3,
// 111 -5, 110 -7
int tribitPhase(int b1, int b2, int b3);
// The pi/8-D8PSK normal training sequences 1 and 2 (9.4.4.3.2, 33 bits each): 1 marks one SCH-P8/F block, 2 two SCH-P8/HD blocks
extern const uint8_t kTrainN8[33], kTrainP8[33];

// ---------------------------------------------------------------- channel coding (clause 8)

// The 16 parity bits of the type-2 block: CRC-CCITT (x^16 + x^12 + x^5 + 1), register preset to ones, the remainder complemented (8.2.3.3)
uint16_t crc16(const uint8_t* b, int n);
// Mother code rate 1/4 (G1 = 1+D+D^4, G2 = 1+D^2+D^3+D^4, G3 = 1+D+D^2+D^4, G4 = 1+D+D^3+D^4), punctured to rate 2/3 (P = 1, 2, 5 of 8)
Bits rcpcEncode23(const Bits& type2);
// Soft Viterbi over the punctured stream: llr > 0 means a 1; the encoder ends in state 0 (the four tail bits). n3 = 3/2 of the type-2 length.
Bits viterbiDecode23(const float* llr3, int n3);
struct ChanSpec;
// Soft-in soft-out (log-MAP) decoding of the same punctured code: the extrinsic LLR of every punctured bit (what the code says about it,
// less what came in), for iterating with the demodulator (tetra_rx.cpp, 3.9). Same conventions as viterbiDecode23.
void sisoDecode23(const float* llr3, int n3, float* ext3);
// The extrinsic LLRs of a logical channel's type-5 bits (sisoDecode23 through the interleaving and the scrambling)
void channelExtrinsic(const float* llr5, const ChanSpec& c, uint32_t scramblerInit, float* ext5);
// Block interleaver (8.2.4.1): type-4 bit k = 1 + (a * i mod K) takes type-3 bit i
void interleave(const uint8_t* in, uint8_t* out, int K, int a);
void deinterleave(const float* in, float* out, int K, int a);
// Scrambling (8.2.5): the 32 stage register filled from the extended colour code (MCC 10, MNC 14, colour code 6 bits) and two ones.
// The synchronisation block (BSCH) uses MCC = MNC = colour code = 0.
uint32_t scramblerInit(int mcc, int mnc, int colourCode);
void scramble(uint8_t* b, int n, uint32_t init);
void descramble(float* llr, int n, uint32_t init);
// Reed-Muller (30,14) of the AACH (8.2.3.2): 14 information bits then 16 parity bits; bit 29 is sent first
uint32_t rm3014Encode(uint16_t info14);
bool rm3014Decode(uint32_t rx30, uint16_t& info14, int* corrected = nullptr);   // corrects up to 3 errors

// A logical channel: its type-1 length, its type-3/4/5 length and the interleaver's a
struct ChanSpec { int type1, K, a; };
constexpr ChanSpec kBsch{60, 120, 11};      // synchronisation (SYNC PDU)
constexpr ChanSpec kSchHd{124, 216, 101};   // half slot signalling (BNCH, SCH/HD, STCH)
constexpr ChanSpec kSchF{268, 432, 103};    // full slot signalling
constexpr ChanSpec kSchP8Hd{196, 324, 19};  // TEDS pi/8-D8PSK half slot downlink signalling (8.3.1, figure 8.7)
constexpr ChanSpec kSchP8F{412, 648, 23};   // TEDS pi/8-D8PSK full slot signalling
// type-1 -> type-5 (CRC, tail, RCPC 2/3, interleave, scramble)
Bits encodeChannel(const Bits& type1, const ChanSpec& c, uint32_t scramblerInit);
// type-5 soft bits (llr > 0 means a 1) -> type-1 bits; false when the CRC fails
bool decodeChannel(const float* llr5, const ChanSpec& c, uint32_t scramblerInit, Bits& type1);
Bits encodeAach(uint16_t accessAssign14, uint32_t scramblerInit);
bool decodeAach(const float* llr30, uint32_t scramblerInit, uint16_t& accessAssign14);

// ---------------------------------------------------------------- bursts (9.4.4.3)

enum class Burst { None = 0, Sync, NormalFull, NormalHalf, NormalFullP8, NormalHalfP8 };   // y; n (one full slot channel); p (two half slot channels);
                                                                                          // the pi/8-D8PSK n and p (TEDS, table 9.14)

// Field positions (0-based bit indices in the 510 bits of the slot)
constexpr int kSbSb = 94, kSbBb = 252, kSbBkn2 = 282;              // synchronisation burst: sb 120, bb 30, bkn2 216
constexpr int kNbBkn1 = 14, kNbBb1 = 230, kNbBb2 = 266, kNbBkn2 = 282;   // normal burst: bkn1 216, bb 14 + 16, bkn2 216
constexpr int kTrainYAt = 214, kTrainNAt = 244;                     // the y and n / p sequences

Bits syncBurst(const Bits& sb120, const Bits& bb30, const Bits& bkn2);
Bits normalBurst(const Bits& bkn1, const Bits& bb30, const Bits& bkn2, bool twoHalves);

// The normal continuous downlink burst for pi/8-D8PSK (table 9.8): 739 bits, the 255 symbols as their phase steps in multiples of pi/8.
// Symbols 1..6, 116..122, 134..141 and 251..255 (training sequence 3 and the broadcast block) stay pi/4-DQPSK; blocks 1 and 2 (324 bits
// each, symbols 8..115 and 142..249), the training sequence (123..133) and the phase adjustment symbols 7 and 250 are pi/8-D8PSK.
constexpr int kP8Bkn1Sym = 8, kP8Bkn2Sym = 142, kP8TrainSym = 123;
std::vector<int> normalBurstP8(const Bits& bkn1_324, const Bits& bb30, const Bits& bkn2_324, bool twoHalves);

// ---------------------------------------------------------------- modulation

// Root raised cosine taps (roll-off 0.35) at sps samples per symbol over +-span symbols, unit energy
std::vector<float> rrcTaps(int sps, int span);

// pi/4-DQPSK modulator with root raised cosine pulse shaping at kSps: bits (a multiple of 2) in, samples out, continuous across calls
class Modulator {
public:
    Modulator();
    void push(const Bits& bits, std::vector<cf32>& out);
    // Symbols given as phase steps in multiples of pi/8 (a TEDS pi/8-D8PSK burst, normalBurstP8): odd steps are pi/8-D8PSK, even ones pi/4-DQPSK
    void pushSteps(const std::vector<int>& stepsPi8, std::vector<cf32>& out);
private:
    void symbol(std::vector<cf32>& out);
    std::vector<float> h_;
    std::vector<cf32> hist_;   // the last symbols (span * 2 + 1)
    int phase_ = 0;            // multiples of pi/8
};

} // namespace dect2::tetra
