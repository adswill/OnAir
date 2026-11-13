// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MAC television (Multiplexed Analogue Components, the MAC/packet family): the numbers and the codes shared by the receiver, the generator
// and the tests. D2-MAC/packet follows ETS 300 250 (EBU SPB 489), read from the specification: line structure (Part 1 Fig. 1a), line and
// frame sync words, the special data of line 625 (FSD, UDT, SDF, RDF with their BCH codes), bit interleaving and energy dispersal, duobinary
// coding, the vision signal (Part 2: 3:2 luma, 3:1 colour difference, U on odd lines), packets (Part 3: Golay (23,12) header, 91 data
// bytes), companded sound (Part 3: 14/10-bit near-instantaneous law, first-level protection, scale factors in the parity bits), the service
// identification channel (Part 5: data groups with Hamming-coded headers, CRC packet suffix, commands and parameters) and the modulation
// (Part 7A: 13.5 MHz/V, linear pre-emphasis, 25 Hz energy dispersal).
// D-MAC is not in that document: its burst (206 bits at 20.25 Mbit/s, one bit per clock period, two 99-bit subframes) is laid out here by
// analogy with D2-MAC and is not checked against EBU Tech. 3258. The energy dispersal generator's initialisation word is not given in the
// copy of the specification read here (all ones is used), nor is the exact W1 / W2 pattern at the frame boundary (Table 1).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace mac {

constexpr double kClockHz = 20.25e6;       // the sample clock of the MAC line (1296 per line)
constexpr int kLineSamples = 1296;
constexpr int kLines = 625;
constexpr double kLineHz = kClockHz / kLineSamples;   // 15625
constexpr double kDevHzPerV = 13.5e6;      // FM deviation sensitivity (Part 7A 2.2, option a)
constexpr double kDispersalHzPp = 600e3;   // energy dispersal, 25 Hz triangle (Part 7A 2.4)
constexpr float kDataAmp = 0.4f;           // duobinary extreme levels at 0.5 +- 0.4 V (80 % of the picture)

enum Variant { kD2 = 0, kD = 1 };
const char* variantName(int v);           // "D2-MAC", "D-MAC"

// The data burst: bits per line (with the 6-bit line sync word), bits of line 625, and where bit i (1 based) sits: the sample number relative to
// sample 1296 of the previous line (= 0). D2: even samples, the first bit on sample 1294 of the previous line; D: every sample from 1291.
inline int burstBits(int v) { return v == kD ? 206 : 105; }
inline int line625Bits(int v) { return v == kD ? 1296 : 648; }
inline int bitSpacing(int v) { return v == kD ? 1 : 2; }
inline int bitSample(int v, int bit) { return v == kD ? bit - 6 : 2 * bit - 4; }
inline int subframes(int v) { return v == kD ? 2 : 1; }
constexpr int kSubframeBits = 99;          // per line and subframe: bits 7..105 (and 106..204 in D-MAC)
constexpr int kPacketBits = 751, kPacketsPerFrame = 82, kPacketBytes = 91;
constexpr int kDataLines = 623;            // lines 1..623 carry the packet multiplex

// line regions (sample numbers 1..1296, Part 1 Fig. 1a)
constexpr int kClampFirst = 211, kClampLast = 225;         // 15 samples at 0.5 V
constexpr int kChromaFirst = 236, kChromaN = 349;          // colour difference 236..584
constexpr int kLumaFirst = 590, kLumaN = 697;              // luminance 590..1286
// lines (Part 2 4.5, Fig. 3): field 1 23..310, field 2 335..622; lines 23 and 335 carry colour difference with black luminance
constexpr int kField1First = 23, kField2First = 335, kFieldRows = 288;
inline bool chromaIsU(int line) { return (line & 1) != 0; }   // E'U on odd lines, E'V on even lines (co-sited with the next line's luminance)
// line 624 references: grey 210..372, white 372..534, black 534..696
constexpr int kRefGrey0 = 222, kRefGrey1 = 360, kRefWhite0 = 384, kRefWhite1 = 522, kRefBlack0 = 546, kRefBlack1 = 684;

// colour difference scaling (Part 2 5.4): E'U = 0.733 (B - Y), E'V = 0.927 (R - Y)
constexpr float kUScale = 0.733f, kVScale = 0.927f;
// pre-emphasis (Part 7A 2.3.1): H(f) = A (1 + j f/f1) / (1 + j f/f2)
constexpr double kPreA = 0.70710678118654752, kPreF1 = 0.84e6, kPreF2 = 1.5e6;

// first-order IIR y = b0 x + b1 x1 - a1 y1 (bilinear transform of (1 + s/wz) / (1 + s/wp) * g)
struct Iir1 {
    float b0 = 1, b1 = 0, a1 = 0, x1 = 0, y1 = 0;
    float run(float x) { const float y = b0 * x + b1 * x1 - a1 * y1; x1 = x; y1 = y; return y; }
    void reset(float x = 0) { const float g = (b0 + b1) / (1 + a1); x1 = x; y1 = g * x; }
};
Iir1 shelf(double fzHz, double fpHz, double gain, double fs);
inline Iir1 preEmphasis(double fs) { return shelf(kPreF1, kPreF2, kPreA, fs); }
inline Iir1 deEmphasis(double fs) { return shelf(kPreF2, kPreF1, 1.0 / kPreA, fs); }
// sound pre-emphasis J.17 (6.5 dB at 800 Hz): zero at 3000 rad/s, pole at 3000 sqrt(75) rad/s, 0 dB at high frequencies
Iir1 j17Pre(double fs);
Iir1 j17De(double fs);

// ---- synchronisation words, transmission order
constexpr uint8_t kLswW1 = 0x0B;           // 001011, the first transmitted bit is the MSB of these 6 bits; W2 = ~W1
int lswBit(bool w1, int i);                // bit i (0..5) of W1 or W2
// the 96-bit frame synchronising sequence of line 625 (clock run-in 55555555, FSW 65AEF3153F41C246), MSB first; inverted on odd frames
int fsdBit(int i);                         // i = 0..95
constexpr uint32_t kClampMarker = 0xEAF3927Fu;   // line 624 (MSB first)

// ---- codes
// Golay (23,12), g(x) = x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1: 12 information bits (transmission order) -> 23 bits
void golay23Encode(const uint8_t info[12], uint8_t out[23]);
int golay23Decode(const uint8_t in[23], uint8_t info[12]);   // corrected errors (0..3); the code is perfect, so always succeeds
// BCH (71,57) / (94,80), shortened (127,113), g(x) = x^14 + x^9 + x^8 + x^6 + x^5 + x^4 + x^2 + x + 1; first transmitted = highest power
void bchEncode(const uint8_t* msg, int k, uint8_t* out);    // out: k message bits then 14 check bits
int bchDecode(uint8_t* cw, int n);                           // corrects one error in place; returns 0 ok, 1 corrected, -1 uncorrectable
// Hamming 8/4 (Part 3 Table 4): nibble -> byte (b1 transmitted first = bit 0), and back (-1: two errors)
uint8_t ham84(int nibble);
int unham84(uint8_t byte);
// CRC x^16 + x^12 + x^5 + 1 over bytes sent LSB first (the packet and data group suffixes)
uint16_t crc16(const uint8_t* bytes, size_t n);
// bit interleaving of a 751-bit packet (Part 1 3.6): transmitted position t carries packet bit perm[t]
const std::vector<int>& interleave();
// energy dispersal PRBS x^15 + x + 1, one element per bit period of the line (648 per line in D2, 1296 in D), from bit 7 of line 1
class Prbs {
public:
    void reset() { s_ = 0x7FFF; }
    int next() { const int o = ((s_ >> 0) ^ (s_ >> 14)) & 1; s_ = ((s_ << 1) | o) & 0x7FFF; return o; }
private:
    uint32_t s_ = 0x7FFF;
};
// the PRBS bits of one frame: index (line - 1) * line625Bits + (bit - 7) for lines 1..625
const std::vector<uint8_t>& prbsFrame(int variant);

// ---- packets
struct Packet {
    int address = 1023, ci = 0;
    uint8_t data[kPacketBytes] = {};
};
void packetBits(const Packet& p, uint8_t out[kPacketBits]);           // header + data, interleaved
// de-interleave and decode; returns the Golay corrections (0..3) of the header
int parsePacket(const uint8_t in[kPacketBits], Packet& p);

// ---- sound: companded (14/10) first-level protection, 64 samples in a 90-byte block (Part 3 6.2, Fig. 4b)
constexpr uint8_t kPtBc1 = 0xC7, kPtBc2 = 0xF8, kPtBi1 = 0x00, kPtBi2 = 0x3F;   // packet type byte (sound / SI CRC: F8 first packet, C7 next)
void soundEncode(const int16_t* x14, bool stereo, uint8_t block[90]);        // x14: 64 samples, -8192 .. 8191
// returns the samples with a parity error (concealed by repeating the previous sample)
int soundDecode(const uint8_t block[90], bool stereo, int16_t* x14, int range[2] = nullptr);

// ---- service identification (packet address 0): data groups of commands
struct SiParam { uint8_t pi = 0; std::vector<uint8_t> field; std::vector<SiParam> group; };   // group: a parameter group (PI 0x80)
struct SiCommand { uint8_t ci = 0; std::vector<SiParam> params; };
// a data group of type tg as the packets carrying it (PT F8 / C7, 88 bytes, CRC suffix)
std::vector<Packet> siDataGroup(int tg, int continuity, const std::vector<SiCommand>& cmds);
std::vector<uint8_t> siEncodeCommands(const std::vector<SiCommand>& cmds);
bool siParseCommands(const uint8_t* b, size_t n, std::vector<SiCommand>& out);
// reassembles data groups from address-0 packets: feed every packet, true when a data group completed (its type and commands in tg / cmds)
class SiAssembler {
public:
    bool push(const Packet& p, int& tg, std::vector<SiCommand>& cmds);
    uint64_t groupsOk = 0, groupsBad = 0, packetsBad = 0, golayPackets = 0;
private:
    std::vector<uint8_t> buf_;
    int want_ = 0, have_ = 0, tg_ = 0, last_ = 0, lastCi_ = -1;
};
// parameter field helpers
std::string siText(const std::vector<uint8_t>& f, size_t from = 0);   // RDS / EBU Latin text (the ASCII part kept)

// PI / CI values (Part 5 Table 1)
constexpr uint8_t kCiNetwork = 0x10, kCiTv = 0x90;
constexpr uint8_t kPiNwo = 0x10, kPiNwname = 0x14, kPiListx = 0x18, kPiSref = 0x40, kPiPg = 0x80, kPiAccm = 0x88, kPiVconf = 0x90, kPiTvSound = 0xA4;
constexpr uint8_t kCaEuroCrypt = 0x40;     // the CA system byte of EuroCrypt (Part 5 Annex 2)
const char* caSystemName(int caByte);      // "EuroCrypt", "VideoGuard" (unverified code) or "conditional access"

// ---- line 625: special data (after the 102-bit FSD)
struct Sdf {
    uint16_t chid = 0;
    uint8_t sdfscr = 0, mvscg = 0xFF;
    uint32_t cafcnt = 0;
    bool sift = true;
    // MVSCG bits (bit 1 first): b4 aspect (1 = 4:3), b6..b8 VSAM
    bool wide() const { return ((mvscg >> 3) & 1) == 0; }
    int vsam() const { return (mvscg >> 5) & 7; }   // bit 6 = LSB
    bool scrambled() const { return (vsam() & 1) == 0; }
    bool controlled() const { return (vsam() >> 2) & 1; }
    bool singleCut() const { return (vsam() >> 1) & 1; }
};
struct Rdf { int tdmcid = 0, fcnt = 0; bool udf = false; int fln1 = 0, lln1 = 0, fln2 = 0, lln2 = 0, fcp = 0, lcp = 0; bool links = false; };
std::string vsamText(const Sdf& s);
// the 546 bits after the FSD: UDT (5), SDF (71), 5 RDF (94 each)
void line625Encode(const Sdf& s, const Rdf rdf[5], uint8_t udt5, uint8_t out[546]);
// returns the number of good blocks (SDF + RDF); sdfOk tells whether the SDF passed its BCH check
int line625Decode(const uint8_t in[546], Sdf& s, Rdf rdf[5], bool& sdfOk);

} // namespace mac
} // namespace dect2
