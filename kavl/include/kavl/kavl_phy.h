// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAVL, the layered narrow-band digital video mode (spec: scratchpad/KAVL-SPEC-v0.1.md): physical layer, transmit side and the shared frame layout.
// Developed as a separate program (kavl/) until it is finished; it then moves into core/ as an OnAir mode.
//
// Goal (owner, 2026-11-23): one 1440p60 video stream with 48 kHz stereo audio in at most 500 kHz at an Es/N0 of 20 dB or less.
//
// One frame = preamble (64 symbols, Zadoff-Chu root 11) + header (64 info bits incl. CRC-16, K = 7 rate 1/2 convolutional code, BPSK, 140 symbols) + data region
// (8100 data symbols with a known +-1 pilot in front of every 32 data symbols and one after the last = 8355 symbols). The 8100 data symbols hold exactly
// bitsPerSymbol / 2 LDPC code words of 16200 bits (ATSC 3.0 A/322 codes): QPSK 1, 16-QAM 2, 64-QAM 3, 256-QAM 4, 1024-QAM 5. The coded bits of the frame are
// permuted together (one pseudo-random permutation per frame size) and scrambled. Each code word carries payloadBytes + CRC-32 (+ up to 7 zero bits).
//
// Single carrier, root-raised-cosine roll-off 0.05; at the default 476 ksym/s the signal occupies 500 kHz. The IQ is produced at 4 samples per symbol; the
// receiver accepts any input rate (it resamples to 4 samples per symbol).
#pragma once
#include "dect2/kris/kris_common.h"
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kavl {

using cf = std::complex<float>;

constexpr int kSps = 4;                       // samples per symbol of the IQ stream
constexpr double kDefaultSymRate = 476000.0;  // symbols per second (500 kHz occupied)
constexpr float kRrcBeta = 0.05f;
constexpr int kRrcSpan = 48;                  // symbols (a small roll-off needs a long filter)
constexpr int kLdpcN = 16200;
constexpr int kDataSyms = 8100;
constexpr int kPilotGroup = 32;               // data symbols per pilot
constexpr int kPilots = (kDataSyms + kPilotGroup - 1) / kPilotGroup + 1;   // 255
constexpr int kRegionSyms = kDataSyms + kPilots;                          // 8355
constexpr int kPreambleSyms = 64;
constexpr int kZcRoot = 11;
constexpr int kHeaderInfoBits = 64;
constexpr int kHeaderCodedBits = (kHeaderInfoBits + 6) * 2;               // 140
constexpr int kHeaderStart = kPreambleSyms;                              // symbol index of the first header symbol
constexpr int kRegionStart = kPreambleSyms + kHeaderCodedBits;           // 204
constexpr int kFrameSyms = kRegionStart + kRegionSyms;                   // 8559
constexpr int kNumModcods = 10;

struct Modcod {
    int id;
    const char* name;
    dect2::kris::Mod mod;
    int bitsPerSym;
    int rate15;        // LDPC rate = rate15 / 15
    int blocks;        // code words per frame = bitsPerSym / 2
    int kBits;         // information bits of one code word
    int payloadBytes;  // payload bytes of one code word (without the CRC-32)
};
// 0 QPSK 8/15, 1 16-QAM 9/15, 2 64-QAM 10/15, 3 256-QAM 10/15, 4 256-QAM 12/15, 5 1024-QAM 10/15, 6 1024-QAM 11/15, 7 1024-QAM 12/15, 8 256-QAM 11/15, 9 64-QAM 12/15.
// nullptr for a bad id.
const Modcod* modcod(int id);
inline int framePayloadBytes(const Modcod& m) { return m.payloadBytes * m.blocks; }
inline double frameSeconds(double symRate) { return kFrameSyms / symRate; }
// Payload bits per second of the PHY (after LDPC, CRC, preamble, header and pilots).
inline double netBitRate(const Modcod& m, double symRate) { return framePayloadBytes(m) * 8.0 * symRate / kFrameSyms; }
// Occupied bandwidth in Hz.
inline double occupiedHz(double symRate) { return symRate * (1.0 + kRrcBeta); }

struct Header {
    int modcod = 0;     // 0..15 (0..7 defined)
    int fec = 0;        // erasure FEC group profile of the container layer, 3 bits
    int layer = 0;      // 0 = single layer (2 bits; reserved for the layered mode)
    int seq = 0;        // frame sequence number, 16 bits, wraps
    int linkQ = 0;      // reported Es/N0 in 0.25 dB steps (8 bits), 0 = unknown
};
// 64 information bits (one per byte), MSB first: version 2 (=0), modcod 4, fec 3, layer 2, seq 16, linkQ 8, reserved 13 (=0), CRC-16 over the first 48.
std::vector<uint8_t> headerBits(const Header& h);
bool parseHeader(const uint8_t* bits64, Header& h);   // false on a CRC or version failure
std::vector<cf> headerSymbols(const Header& h);       // 140 BPSK symbols

const std::vector<cf>& preambleSymbols();             // 64 symbols, unit magnitude
float pilotValue(int i);                              // +1 or -1, i = 0 .. kPilots - 1

// Data region layout: the region index of every pilot and data symbol.
struct RegionMap {
    std::vector<int> pilot;   // kPilots entries
    std::vector<int> data;    // kDataSyms entries, in order
};
const RegionMap& regionMap();

// Frame bit permutation + scrambling (nBits = blocks x 16200). Code order -> transmitted order.
void interleaveScramble(const uint8_t* in, uint8_t* out, int nBits);
// LLRs in transmitted order -> code order (descrambling flips the sign).
void deinterleaveDescrambleLlr(const float* inTx, float* outCode, int nBits);

// Coded bits of a frame in transmitted order: payload = framePayloadBytes(m) bytes.
std::vector<uint8_t> frameCodedBits(const Modcod& m, const uint8_t* payload);
// Symbols of a whole frame (kFrameSyms).
std::vector<cf> frameSymbols(const Modcod& m, const Header& h, const uint8_t* payload);
// Symbols of a frame from already mapped data symbols (kDataSyms).
std::vector<cf> assembleFrame(const Header& h, const std::vector<cf>& dataSyms);

// Root-raised-cosine taps (unit energy) of the transmitter and the receiver's matched filter.
const std::vector<float>& rrc();

class Transmitter {
public:
    explicit Transmitter(int modcodId);
    const Modcod& modcod() const { return *m_; }
    // IQ of the frames back to back (4 samples per symbol, data power 1 per sample): leadSyms of silence, the frames, tailSyms of silence.
    // frames[i] = framePayloadBytes bytes; sequence numbers count from startSeq.
    std::vector<cf> modulate(const std::vector<std::vector<uint8_t>>& frames, int fec = 0, int linkQ = 0, int startSeq = 0, int leadSyms = 32,
                             int tailSyms = 32) const;
    // Pulse shaping of a symbol stream (4 samples per symbol, data power 1 per sample).
    static std::vector<cf> shape(const std::vector<cf>& syms);
private:
    const Modcod* m_;
};

}  // namespace kavl
