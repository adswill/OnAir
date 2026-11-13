// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAFT physical layer: the profile and width tables, block packing, frame / burst builder, preamble, header, RRC shaping.
// Spec: KAFT-SPEC-v0.1 (ten profiles P0-P9, widths 200 kHz / 500 kHz / 1 MHz, roll-off 0.25). The error coding (LDPC n = 7680, convolutional
// header code, QAM mapping, CRCs, Zadoff-Chu, RRC taps) is the shared foundation in dect2/kris/.
//
// Decisions where the spec is inconsistent (see also the commit message):
//  - Block accounting: the spec asks for whole 256-byte chunks per block, which cannot work at P0 / P1 (payload 236 bytes < 256). A block carries
//    payloadBytes = (K - 32) / 8 bytes; the first 6 are a block header (u32 byte offset in the file, u16 length), the rest is file data. The unit
//    of selective repeat and of the resume map is therefore a byte range, not a fixed chunk (RangeSet in kaft_session.h). The CRC-32 is the last
//    4 information bytes of the block.
//  - Header: 256 BPSK symbols, convolutional rate 1/2 (122 information bits), not rate 1/4 + repetition: the callsign (6 characters, 6 bits each),
//    width, profile, session id, frame number, block count, burst position and a CRC-16 need about 105 bits, rate 1/4 gives only 58.
//  - Preamble: the spec says BPSK, but a Zadoff-Chu sequence is complex: guard (last 10 symbols of the sequence) + ZC(251) + ZC(251) = 512 symbols.
//  - Control frames and ack frames use P1 (QPSK 1/4) blocks, as many as the message needs (not padded to 24000 / 8000 symbols).
#pragma once
#include "dect2/kris/kris_common.h"
#include "dect2/kris/kris_fec.h"
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dect2::kaft {

using cf = std::complex<float>;

// ---- widths ----
enum class Width : uint8_t { W1M = 0, W500k = 1, W200k = 2 };   // the 2-bit width code of the spec: 00 = 1 MHz, 01 = 500 kHz, 10 = 200 kHz
constexpr int kNumWidths = 3;
constexpr int kSps = 4;                       // internal samples per symbol
constexpr int kRefRate = 3200000;             // reference sample rate of the waveform exchanged with a radio (4 x 800 ksym/s); every width divides it
struct WidthInfo {
    Width id;
    const char* name;
    int symRate;        // symbols per second
    int frameMs;        // frame length of the spec
    int zcRoot;         // Zadoff-Chu root of the preamble (also identifies the width)
    int refFactor;      // kRefRate / (kSps * symRate): the integer ratio between the reference rate and the internal 4 sps rate
    int frameSymbols;   // symbol budget of one data frame (symRate * frameMs)
};
inline constexpr WidthInfo kWidths[kNumWidths] = {
    {Width::W1M, "1 MHz", 800000, 40, 13, 1, 32000},
    {Width::W500k, "500 kHz", 400000, 80, 11, 2, 32000},
    {Width::W200k, "200 kHz", 160000, 100, 7, 5, 16000},
};
constexpr const WidthInfo& widthInfo(Width w) { return kWidths[(int)w]; }

// ---- profiles ----
constexpr int kNumProfiles = 10;
struct ProfileInfo {
    int id;
    const char* name;
    kris::Mod mod;
    kris::LdpcRate rate;
    double rateValue;
    double reqEsN0Db;   // FER ~1e-2. P0 and P1 are measured (LDPC n = 7680, AWGN, max-log, 50 iterations, 600 codewords, 0.25 dB steps): BPSK 1/4 -0.75 dB, QPSK 1/4 +2.25 dB; the others are the spec table, an estimate (Shannon + 1.5 dB), optimistic
};
inline constexpr ProfileInfo kProfiles[kNumProfiles] = {
    {0, "P0 BPSK 1/4", kris::Mod::Bpsk, kris::LdpcRate::R1_4, 0.25, -0.75},
    {1, "P1 QPSK 1/4", kris::Mod::Qpsk, kris::LdpcRate::R1_4, 0.25, 2.25},
    {2, "P2 QPSK 1/2", kris::Mod::Qpsk, kris::LdpcRate::R1_2, 0.5, 1.5},
    {3, "P3 QPSK 3/4", kris::Mod::Qpsk, kris::LdpcRate::R3_4, 0.75, 4.1},
    {4, "P4 16-QAM 3/4", kris::Mod::Qam16, kris::LdpcRate::R3_4, 0.75, 10.0},
    {5, "P5 64-QAM 3/4", kris::Mod::Qam64, kris::LdpcRate::R3_4, 0.75, 14.9},
    {6, "P6 256-QAM 3/4", kris::Mod::Qam256, kris::LdpcRate::R3_4, 0.75, 19.8},
    {7, "P7 256-QAM 5/6", kris::Mod::Qam256, kris::LdpcRate::R5_6, 5.0 / 6.0, 21.8},
    {8, "P8 1024-QAM 5/6", kris::Mod::Qam1024, kris::LdpcRate::R5_6, 5.0 / 6.0, 26.9},
    {9, "P9 1024-QAM 9/10", kris::Mod::Qam1024, kris::LdpcRate::R9_10, 0.9, 28.9},
};
constexpr const ProfileInfo& profileInfo(int p) { return kProfiles[p < 0 ? 0 : (p >= kNumProfiles ? kNumProfiles - 1 : p)]; }

// ---- frame constants ----
constexpr int kBlockBits = 7680;        // coded bits of one LDPC block
constexpr int kCrcBits = 32;
constexpr int kBlockHdrBytes = 6;       // u32 file offset + u16 length at the start of a data block
constexpr int kPreambleSyms = 512;
constexpr int kZcLen = 251;
constexpr int kZcGuard = 10;
constexpr int kHeaderSyms = 256;
constexpr int kHeaderInfoBits = 122;    // 256 / 2 - 6 (rate 1/2, constraint length 7, terminated)
constexpr int kPilotPeriod = 32;        // one pilot, then 31 data symbols
constexpr int kFramesPerBurst = 16;
constexpr int kCtrlProfile = 1;         // control and ack frames
constexpr int kCtrlMaxBlocks = 5;
constexpr int kRrcSpan = 20;            // RRC span in symbols
constexpr float kRrcBeta = 0.25f;

enum class FrameType : uint8_t { Data = 0, Ctrl = 1, Ack = 2 };

struct FrameHeader {
    Width width = Width::W1M;
    int profile = 0;
    FrameType type = FrameType::Data;
    uint16_t session = 0;
    uint16_t frameNo = 0;
    uint8_t nBlocks = 0;
    uint8_t frameInBurst = 0;   // 0-15
    bool lastInBurst = false;
    std::string callsign;       // at most 6 characters of A-Z 0-9 / - .
};

// ---- block accounting (all derived from kris::ldpcK) ----
int bitsPerSymbol(int profile);
int blockInfoBits(int profile);                        // K of the LDPC code
int payloadBytes(int profile);                         // (K - 32) / 8: what one block carries (block header included)
int dataBytesPerBlock(int profile);                    // payloadBytes - kBlockHdrBytes
int symbolsPerBlock(int profile);                      // 7680 / bits per symbol, pilots not included
int payloadSymbols(int profile, int nBlocks);          // data symbols of nBlocks blocks plus the pilots
int frameSymbolCount(int profile, int nBlocks);        // preamble + header + payload
int maxBlocksPerFrame(int profile, Width w);           // blocks of a data frame that fit the frame length of the width (3 to 39 at 1 MHz)
double frameSeconds(Width w, int profile, int nBlocks);

// ---- coding of one block ----
// payload: at most payloadBytes(profile) bytes (zero padded); returns the 7680 coded bits (one per byte, 0 / 1)
std::vector<uint8_t> encodeBlock(int profile, uint16_t frameNo, int blockIdx, const uint8_t* payload, size_t len);
// llr: 7680 values; on a CRC pass fills payload with payloadBytes(profile) bytes and returns true
bool decodeBlock(int profile, uint16_t frameNo, int blockIdx, const float* llr, std::vector<uint8_t>& payload);

// data block header
std::vector<uint8_t> packDataBlock(uint32_t offset, const uint8_t* data, size_t len);   // 6 byte header + data (len <= dataBytesPerBlock is the caller's job)
bool unpackDataBlock(const std::vector<uint8_t>& payload, uint32_t& offset, std::vector<uint8_t>& data);

// ---- header ----
std::vector<uint8_t> encodeHeaderBits(const FrameHeader& h);                         // kHeaderInfoBits bits
bool decodeHeaderBits(const std::vector<uint8_t>& bits, FrameHeader& h);            // CRC-16 checked
std::vector<cf> headerSymbols(const FrameHeader& h);                                 // kHeaderSyms BPSK symbols

// ---- preamble ----
const std::vector<cf>& preambleSymbols(Width w);                                     // kPreambleSyms symbols

// ---- frames ----
struct FrameSpec {
    FrameHeader header;
    std::vector<std::vector<uint8_t>> blocks;   // payloadBytes(header.profile) bytes each (shorter ones are zero padded); header.nBlocks is set by the builder
};
// symbols at the symbol rate (preamble, header, pilots and data); empty when the spec is invalid (too many blocks, bad profile)
std::vector<cf> frameSymbols(const FrameSpec& f);
// RRC shaping at 4 samples per symbol, unit average power; the output is longer than 4 x symbols by the filter length
std::vector<cf> rrcShape(const std::vector<cf>& symbols);
int rrcDelaySamples();                                                               // group delay of the transmit filter in samples (4 sps)
std::vector<cf> buildFrame(const FrameSpec& f);                                      // 4 sps waveform of one frame
std::vector<cf> buildBurst(const std::vector<FrameSpec>& frames);                    // frames back to back, shaped as one symbol stream

// ---- resampling (the hook) ----
// Rational resampler (up / down) with a centred windowed-sinc filter, zero delay. The default of everything below; a radio driver can supply its own.
std::vector<cf> resample(const std::vector<cf>& x, int up, int down);
using ResampleFn = std::function<std::vector<cf>(const std::vector<cf>&, int up, int down)>;
// 4 sps waveform of a width to the reference rate (kRefRate) and back
std::vector<cf> toReferenceRate(const std::vector<cf>& x4sps, Width w, const ResampleFn& fn = nullptr);

// helpers shared with the receiver
const std::vector<float>& rrcFilter();                  // transmit / matched filter taps, scaled to sum of squares = kSps
std::vector<cf> pilotSymbols(size_t n);                  // the known pilot sequence (+1 / -1)
uint64_t blockScramblerSeed(uint16_t frameNo, int blockIdx);
std::vector<uint8_t> packBits(const std::vector<uint8_t>& bits);   // MSB first
std::vector<uint8_t> unpackBits(const uint8_t* bytes, size_t nBytes);
std::vector<uint8_t> callsignBits(const std::string& cs);           // 36 bits
std::string callsignFromBits(const uint8_t* bits);

}  // namespace dect2::kaft
