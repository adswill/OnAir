// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAVB, the amateur digital video mode (spec: KAVB-SPEC-v0.1): the physical layer, transmit side and the shared frame layout.
//
// One frame = preamble (32 symbols, Zadoff-Chu root 7) + header (64 info bits incl. CRC-16, K = 7 rate 1/2 convolutional code, BPSK, 140 symbols) + data region
// (4 LDPC blocks n = 1920 rate 3/4, bit-interleaved across the 4 blocks, scrambled, QAM mapped, one known +-1 pilot in front of every 15 data symbols and one
// after the last). Single carrier, root-raised-cosine roll-off 0.25. The IQ is produced at 4 samples per symbol (sample rate = 4 x symbol rate); the caller
// resamples to the radio rate.
//
// Profiles (symbol rate / modulation): A 700 ksym/s 16-QAM, B 1050 ksym/s 16-QAM, C 1400 ksym/s 16-QAM, D 1400 ksym/s 64-QAM, all rate 3/4. The robust mode (header flag) is QPSK
// rate 1/2 at the symbol rate of the profile. The header carries the profile, so a receiver of the right symbol rate needs no setting (C and D share a rate).
//
// V-shaped sweep signature: a unit-amplitude chirp scaled to 10% of the data power (-10 dB) whose frequency falls linearly from +0.45 to -0.45 symbol rates and rises back
// again, one V per period. The period is a whole number K of frames (K = round(0.5 s x symbol rate / frame symbols), so about 0.5 s); the header carries the frame's index
// inside the period (10 bits), which tells the receiver the chirp phase, so it can estimate and subtract the chirp before the matched filter.
//
// Details the spec leaves open, as built here: block payload 178 bytes (118 in robust mode) + CRC-16 = the 1440 (960) information bits; the coded bits are XORed with a fixed
// pseudo-random sequence; header fields below; callsign/beacon slice (spec section 5) is not part of this layer.
#pragma once
#include "dect2/kris/kris_common.h"
#include "dect2/kris/kris_fec.h"
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2::kavb {

using cf = std::complex<float>;

constexpr int kSps = 4;                     // samples per symbol of the IQ stream
constexpr int kRrcSpan = 12;                // symbols
constexpr float kRrcBeta = 0.25f;
constexpr int kBlockN = 1920;
constexpr int kBlocksPerFrame = 4;
constexpr int kFrameCodedBits = kBlockN * kBlocksPerFrame;   // 7680
constexpr int kPreambleSyms = 32;
constexpr int kZcRoot = 7;
constexpr int kHeaderInfoBits = 64;
constexpr int kHeaderCodedBits = (kHeaderInfoBits + 6) * 2;   // 140
constexpr int kPilotGroup = 15;             // data symbols per pilot
constexpr double kChirpPeriodSec = 0.5;
constexpr double kChirpPowerRel = 0.1;      // chirp power relative to the data power
constexpr double kChirpSpan = 0.45;         // peak frequency, symbol rates
constexpr int kMaxSweepIdx = 1023;
constexpr int kNumProfiles = 4;

struct Profile {
    int id;                 // 0..3 = A..D
    char name;
    double symRate;
    kris::Mod mod;          // the normal mode
    bool robust;            // QPSK rate 1/2
    kris::LdpcRate rate;
    int bitsPerSym;
    int blockPayloadBytes;  // bytes of one block without its CRC-16
    int dataSyms;           // data symbols of a frame (without pilots)
    int regionSyms;         // data symbols + pilots
    int frameSyms;          // preamble + header + region
    int sweepFrames;        // K: frames per chirp period
};
Profile profileOf(int id, bool robust);
inline int framePayloadBytes(const Profile& p) { return p.blockPayloadBytes * kBlocksPerFrame; }
inline double frameSeconds(const Profile& p) { return p.frameSyms / p.symRate; }

struct FrameHeader {
    int profile = 0;        // 0..3
    bool robust = false;
    int fec = 0;            // FEC group profile of the container layer (3 bits, kavb_av.h)
    int sweepIdx = 0;       // frame index inside the chirp period, 0..1023
    int seq = 0;            // frame sequence number, 16 bits, wraps
    int linkQ = 0;          // reported Es/N0 in 0.25 dB steps from 0 dB (8 bits), 0 = unknown
};
// 64 information bits (one per byte), MSB first: version 2 (=0), profile 2, robust 1, fec 3, sweepIdx 10, seq 16, linkQ 8, reserved 6 (=0), CRC-16 over the first 48.
std::vector<uint8_t> headerBits(const FrameHeader& h);
// False when the CRC-16 or the version fails.
bool parseHeader(const uint8_t* bits64, FrameHeader& h);
// 140 BPSK symbols of the header.
std::vector<cf> headerSymbols(const FrameHeader& h);

// The 32 preamble symbols.
const std::vector<cf>& preambleSymbols();

// Positions inside the data region (0 .. regionSyms): pilots and data.
struct RegionMap {
    std::vector<int> pilot;   // region index of each pilot
    std::vector<int> data;    // region index of each data symbol, in order
};
const RegionMap& regionMap(int dataSyms);
// Pilot value of pilot number i (+1 or -1).
float pilotValue(int i);

// Interleaver / scrambler of the frame's coded bits.
void interleaveScramble(const uint8_t* in7680, uint8_t* out7680);                 // coded -> transmitted order
// Soft version: LLRs in transmitted order -> LLRs in code order (descramble flips the sign, deinterleave permutes).
void deinterleaveDescrambleLlr(const float* inTx, float* outCode);

// The coded bits of a frame (7680, transmitted order): 4 blocks of payload + CRC-16, LDPC, interleaved, scrambled. payload = framePayloadBytes(p) bytes.
std::vector<uint8_t> frameCodedBits(const Profile& p, const uint8_t* payload);
// The symbols of a whole frame: preamble, header, data region with pilots.
std::vector<cf> frameSymbols(const Profile& p, const FrameHeader& h, const uint8_t* payload);
// Same layout from already mapped data symbols (for the receiver's training).
std::vector<cf> assembleFrame(const Profile& p, const FrameHeader& h, const std::vector<cf>& dataSyms);

// Chirp of unit amplitude at position `symPos` (symbols) from the start of the chirp period; period = periodSyms symbols.
cf chirpUnit(double symPos, double periodSyms);

class Transmitter {
public:
    // chirp = add the V-shaped sweep signature.
    Transmitter(int profileId, bool robust = false, bool chirp = true);
    const Profile& profile() const { return p_; }
    // IQ of the frames sent back to back: leadSyms of silence, the frames, tailSyms of silence. frames[i] = framePayloadBytes bytes. The frame sequence numbers
    // count from startSeq, the sweep index from startSweep. Sample rate = 4 x symRate, data power 1 per sample.
    std::vector<cf> modulate(const std::vector<std::vector<uint8_t>>& frames, int fec = 0, int linkQ = 0, int startSeq = 0, int startSweep = 0,
                             int leadSyms = 32, int tailSyms = 32) const;
private:
    Profile p_;
    bool chirp_;
};

}  // namespace dect2::kavb
