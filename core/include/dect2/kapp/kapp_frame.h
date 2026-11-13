// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAPP, the amateur packet protocol (spec: KAPP-SPEC-v0.1): the burst physical layer, transmit side.
//
// A burst = ramp up (8) + preamble (two Zadoff-Chu sequences, roots 41 and 86, 127 symbols each) + header (64 info bits + CRC-16, K = 7 rate 1/2
// convolutional code, sent 4 times, BPSK) + payload (LDPC blocks N = 2304, one CRC-16 per block, a +1 pilot after every 31 data symbols) + ramp down (8).
// 80 ksym/s, root-raised-cosine roll-off 0.25. The internal sample rate is 8 samples per symbol (640 ksps); the Resampler converts to and from the radio rate.
//
// Details the spec leaves open, as built here:
//  * the coded bits of block b are XORed with a fixed pseudo-random sequence (scramblerSequence) so that an all-zero payload does not make a carrier line;
//  * a block whose bits do not fill whole symbols (1024QAM: 2304 / 10) is padded with zero bits up to the next symbol (231 symbols);
//  * header flag bits (4-bit field, MSB first): bit 0 ack-request, bit 1 retransmit-burst, bit 2 final-burst, bit 3 = "this is an ACK burst" (the reserved bit);
//  * callsign: 7 characters base 37 (A-Z, 0-9, '/'); shorter calls are padded with trailing '/', which decode strips; longer calls are cut to 7 characters.
#pragma once
#include "dect2/kris/kris_common.h"
#include "dect2/kris/kris_fec.h"
#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::kapp {

using cf = std::complex<float>;

constexpr double kSymRate = 80000.0;
constexpr int kSps = 8;                               // internal samples per symbol
constexpr double kInternalRate = kSymRate * kSps;     // 640 ksps
constexpr int kRampSyms = 8;
constexpr int kZcLen = 127;
constexpr int kZcRoot1 = 41;
constexpr int kZcRoot2 = 86;
constexpr int kPreambleSyms = 2 * kZcLen;             // 254
constexpr int kHeaderInfoBits = 80;                   // 64 + CRC-16
constexpr int kHeaderCodedBits = 172;                 // (80 + 6 tail) x 2
constexpr int kHeaderRepeat = 4;
constexpr int kHeaderSyms = kHeaderCodedBits * kHeaderRepeat;   // 688
constexpr int kFixedOverheadSyms = 2 * kRampSyms + kPreambleSyms + kHeaderSyms;   // 958
constexpr int kBlockN = 2304;
constexpr int kBlockCrcBits = 16;
constexpr int kPilotGroup = 31;                       // data symbols between pilots
constexpr int kPilotPeriod = kPilotGroup + 1;
constexpr int kMaxBlocks = 1023;                      // 10-bit block count
constexpr int kAckMaxBlocks = 512;
constexpr int kMaxProfileId = 8;
constexpr int kRrcSpan = 12;                          // symbols
constexpr float kRrcBeta = 0.25f;

// header flags
constexpr int kFlagAckRequest = 1;
constexpr int kFlagRetransmit = 2;
constexpr int kFlagFinal = 4;
constexpr int kFlagAck = 8;

struct Profile {
    int id;
    kris::Mod mod;
    kris::LdpcRate rate;
    int bitsPerSym;
    int payloadBytes;      // (K - 16) / 8
    float needEsN0Db;      // FER ~1e-2. P0 and P1 are measured (LDPC n = 2304, AWGN, max-log, 50 iterations, 600 codewords, 0.25 dB steps; tests/kris/test_kris_thresholds.cpp method): BPSK 1/4 -1.0 dB, QPSK 1/4 +2.0 dB; the others are spec estimates (Shannon + 1.5 dB) and are optimistic
};

inline constexpr std::array<Profile, 9> kProfiles{{
    {0, kris::Mod::Bpsk, kris::LdpcRate::R1_4, 1, 70, -1.0f},
    {1, kris::Mod::Qpsk, kris::LdpcRate::R1_4, 2, 70, 2.0f},
    {2, kris::Mod::Qpsk, kris::LdpcRate::R1_2, 2, 142, 1.5f},
    {3, kris::Mod::Qpsk, kris::LdpcRate::R3_4, 2, 214, 4.1f},
    {4, kris::Mod::Qam16, kris::LdpcRate::R1_2, 4, 142, 6.3f},
    {5, kris::Mod::Qam16, kris::LdpcRate::R3_4, 4, 214, 10.0f},
    {6, kris::Mod::Qam64, kris::LdpcRate::R2_3, 6, 190, 13.3f},
    {7, kris::Mod::Qam256, kris::LdpcRate::R3_4, 8, 214, 19.5f},
    {8, kris::Mod::Qam1024, kris::LdpcRate::R5_6, 10, 238, 26.6f},
}};

const Profile* profileById(int id);                   // nullptr for reserved ids (9-15) and out of range
inline int infoBitsPerBlock(const Profile& p) { return p.payloadBytes * 8 + kBlockCrcBits; }   // K
int symbolsPerBlock(const Profile& p);                // ceil(2304 / bits per symbol)
int dataSymbols(const Profile& p, int nBlocks);
int payloadSymbols(const Profile& p, int nBlocks);    // data + pilots
int burstSymbols(const Profile& p, int nBlocks);      // whole burst incl. ramps, preamble, header
int maxBlocksForSymbols(const Profile& p, long burstSyms);   // spec: floor((T - 958) * 31/32 / symbolsPerBlock)
int maxBlocksForMs(const Profile& p, double ms);
// true when kris::ldpcK agrees with the table for every profile (the table must match the code)
bool profileTableConsistent();

// ---- callsign ----
uint64_t encodeCallsign(const std::string& call);     // 40 bits
std::string decodeCallsign(uint64_t v);               // "" when v is not a valid 7-character code
bool callsignCodeValid(uint64_t v);

// ---- header ----
struct BurstHeader {
    std::string callsign;
    int profile = 0;
    int blocks = 0;
    int seq = 0;       // 6 bits
    int flags = 0;     // 4 bits
};
std::vector<uint8_t> headerInfoBits(const BurstHeader& h);                   // 80 bits (one per byte) incl. CRC-16
bool parseHeaderBits(const uint8_t* bits80, BurstHeader& h);                 // false when the CRC or the callsign code is wrong
std::vector<cf> headerSymbols(const BurstHeader& h);                         // 688 BPSK symbols (bit 0 = +1)

// ---- payload ----
std::vector<uint8_t> packBits(const uint8_t* bits, size_t n);                // MSB first, one bit per input byte
std::vector<uint8_t> scramblerSequence(int blockIdx, int nBits);
const std::vector<cf>& preambleSymbols();                                    // root 41 then root 86, 254 symbols
const std::vector<float>& rrcShape();                                        // transmit taps, sum of squares = kSps (unit power out)

struct BurstSpec {
    std::string callsign;
    int profileId = 2;
    int seq = 0;
    int flags = 0;
    std::vector<uint8_t> payload;      // split into blocks of Profile::payloadBytes, last one zero padded
};

// the symbols at the symbol rate (ramps, preamble, header, payload with pilots); empty when the spec is invalid
std::vector<cf> buildBurstSymbols(const BurstSpec& spec);
// the burst at the internal rate (640 ksps), RRC shaped, unit average power; empty when the spec is invalid
std::vector<cf> buildBurst(const BurstSpec& spec);

// ---- ACK burst: profile 0, one block = 6 status bits (SNR) + bitmap of up to 512 blocks ----
struct AckInfo {
    std::string callsign;            // the receiver's call
    int seq = 0;                     // sequence number of the burst being acknowledged
    float snrDb = 0.f;               // measured Es/N0
    std::vector<uint8_t> blockOk;    // 1 per block of that burst (only the first 512 are carried)
};
std::vector<uint8_t> encodeAckPayload(float snrDb, const std::vector<uint8_t>& blockOk);   // 70 bytes
bool decodeAckPayload(const uint8_t* data, size_t n, float& snrDb, std::vector<uint8_t>& bitmap512);
BurstSpec ackSpec(const AckInfo& a);
std::vector<cf> buildAck(const AckInfo& a);

// ---- sample rate conversion (windowed sinc, streaming) ----
class Resampler {
public:
    Resampler(double inRate, double outRate);
    std::vector<cf> process(const cf* x, size_t n);
    std::vector<cf> flush();                         // pushes zeros through so the tail comes out
private:
    double step_ = 1.0;
    int half_ = 8;
    std::vector<float> table_;
    std::vector<cf> buf_;
    long long base_ = 0;
    double t_ = 0.0;
};
std::vector<cf> resample(const std::vector<cf>& x, double inRate, double outRate);

}  // namespace dect2::kapp
