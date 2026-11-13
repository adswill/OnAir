// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meteor-M LRPT, the layers above the demodulator (both directions, so the test signal is built with the same code it is checked by).
//
// Downlink: CCSDS-like. Soft bits -> (N2-x 80k: marker removal and convolutional deinterleaver, 36 branches x 2048) -> Viterbi K=7
// r=1/2 (polynomials 0x4F / 0x6D, no inverted output, the encoder runs on without resets) -> (N2-x: NRZ-M decoding of the bits) ->
// 1024-byte CADU found by its sync word 0x1ACFFC1D -> derandomise (CCSDS PN, x^8+x^7+x^5+x^3+1) -> Reed-Solomon (255,223) in the
// conventional basis (not the CCSDS dual basis), interleave 4 ->
// VCDU (892 bytes) -> M_PDU (882 bytes, first header pointer) -> CCSDS space packets -> MSU-MR packets of APID 64 - 69 (channels 1 - 6):
// an 8-byte time stamp, the MCU number, a scan and a segment header (quality factor), then 14 8x8 blocks coded as baseline JPEG
// (the standard luminance Huffman tables, the DC predictor restarting in each packet, the standard quantisation table scaled by the
// quality factor) -> a strip of 8 lines by 112 pixels; 14 packets make the 1568-pixel line. A period of 43 packets (14 per channel for
// three channels and one of telemetry) shares one sequence counter.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace dect2::meteor {

constexpr uint32_t kAsm = 0x1ACFFC1Du;
constexpr int kCaduBytes = 1024;               // ASM + 1020
constexpr int kCodedBytes = 1020;              // 4 x 255
constexpr int kVcduBytes = 892;                // 4 x 223
constexpr int kMpduData = 882;
constexpr int kFrameBits = kCaduBytes * 8;     // info bits a frame
constexpr int kImageWidth = 1568;
constexpr int kMcuPerPacket = 14, kMcuPerLine = 196;
constexpr int kFirstApid = 64, kNumChannels = 6;
constexpr int kInterBranches = 36, kInterDelay = 2048, kMarkerStride = 80, kMarkerData = 72;
constexpr uint8_t kMarker = 0x27;

// --- convolutional code --------------------------------------------------------------------------------------------------------
struct ConvEncoder {
    uint8_t sr = 0;
    // two code bits (0/1) for one info bit
    void push(int bit, uint8_t& a, uint8_t& b);
};
// The 64 code bits of the sync word, the encoder's state before it being unknown: bits 12 .. 63 do not depend on it.
uint64_t encodedAsm();

// Soft-decision Viterbi, continuous (traceback 96). Soft values: > 0 is a 1, magnitude the confidence.
class Viterbi27 {
public:
    Viterbi27() { reset(); }
    void reset();
    void push(int a, int b);                          // one pair of soft code bits
    // decided bits, oldest first, as they become final (about 96 bits late)
    std::vector<uint8_t>& out() { return out_; }
    void flush();                                     // decide everything pushed (end of a test buffer)
private:
    void traceback(int emit, bool all);
    std::array<int32_t, 64> m_{};
    std::vector<uint64_t> dec_;                       // one survivor bit per state per step
    std::vector<uint8_t> out_;
    size_t steps_ = 0;
};

// --- Reed-Solomon (255,223) CCSDS ----------------------------------------------------------------------------------------------------
// dual: symbols in the CCSDS dual (Berlekamp) basis, as GOES LRIT / HRIT send them. Meteor-M LRPT does not: its codewords are in the
// conventional basis (SatDump and meteor_decode decode them with the conversion off), so the Meteor code passes dual = false.
void rsEncode(const uint8_t data[223], uint8_t parity[32], bool dual = true);
int rsDecode(uint8_t cw[255], bool dual = true);                      // in place; errors corrected, -1 when it cannot

// --- frame ----------------------------------------------------------------------------------------------------------------------------
void pnXor(uint8_t* p, size_t n);                                     // the CCSDS sequence from 0xFF, 0x48, 0x0E, 0xC0 ...
// The 1020 bytes after the ASM: derandomise, deinterleave, correct. vcdu gets the 892 data bytes. Returns the errors corrected (sum
// of the four codewords) or -1 when one codeword could not be corrected.
int decodeCadu(const uint8_t coded[kCodedBytes], uint8_t vcdu[kVcduBytes], bool dual = true);
void encodeCadu(const uint8_t vcdu[kVcduBytes], uint8_t cadu[kCaduBytes], bool dual = true);   // ASM, RS, randomised
// NRZ-M (the N2-x differential code, on the bits before the convolutional encoder): out = in ^ previous in / out = in ^ previous out
struct NrzmDecoder { uint8_t prev = 0; uint8_t push(uint8_t b) { const uint8_t o = (uint8_t)(b ^ prev); prev = b; return o; } };
struct NrzmEncoder { uint8_t prev = 0; uint8_t push(uint8_t b) { prev = (uint8_t)(b ^ prev); return prev; } };

// --- VCDU / M_PDU / packets -------------------------------------------------------------------------------------------------------
struct Vcdu {
    int version = 0, scid = 0, vcid = 0;
    uint32_t counter = 0;
    int fhp = 0x7FF;                                  // first header pointer: 0x7FF no packet starts here
    const uint8_t* data = nullptr;                    // kMpduData bytes
};
bool parseVcdu(const uint8_t v[kVcduBytes], Vcdu& out);
void buildVcduHeader(uint8_t v[kVcduBytes], int scid, int vcid, uint32_t counter, int fhp);

struct Packet {
    int apid = 0;
    int seq = 0;                                      // 14 bits
    std::vector<uint8_t> data;                        // user data (after the 6-byte header)
};

// Reassembles space packets across M_PDUs of one virtual channel; a gap in the VCDU counter drops the packet being built.
class PacketDemux {
public:
    std::function<void(const Packet&)> onPacket;
    void reset() { buf_.clear(); have_ = false; lastCounter_ = -1; }
    void push(const Vcdu& v);
    uint64_t packets = 0, dropped = 0;
private:
    void take(const uint8_t* p, size_t n);
    std::vector<uint8_t> buf_;
    bool have_ = false;                               // buf_ holds the start of a packet
    int64_t lastCounter_ = -1;
};

// Packs packets into M_PDU zones (the test signal)
class PacketMux {
public:
    void add(int apid, int seq, const std::vector<uint8_t>& userData);
    bool next(uint8_t vcdu[kVcduBytes], int scid, int vcid);         // false when less than a zone is waiting (pads only with flush)
    void flush();                                                    // pad the last zone with an idle packet
private:
    std::vector<uint8_t> q_;
    std::vector<size_t> starts_;                                     // packet starts in q_
    uint32_t counter_ = 0;
};

// --- MSU-MR image packets ---------------------------------------------------------------------------------------------------------
struct McuStrip {
    int mcu = 0;                                      // first MCU (0, 14, .. 182)
    int q = 0;                                        // quality factor
    uint32_t day = 0, ms = 0;                         // on-board time
    uint8_t px[8][kMcuPerPacket * 8] = {};
};
bool decodeImagePacket(const std::vector<uint8_t>& user, McuStrip& out);   // false: not image data or broken
std::vector<uint8_t> encodeImagePacket(const uint8_t px[8][kMcuPerPacket * 8], int mcu, int q, uint32_t day, uint32_t ms);
void qTable(int q, int out[64]);                     // natural order

// The picture row (strip of 8 lines) of each image packet. Once the time of a row has been seen in the stream (two rows in order),
// the on-board time stamps place the packets: they survive any gap, the 14-bit counter wrapping and a row whose first channel was lost.
// Before that, and when the time goes backwards: a channel whose block position does not move on starts a new row, and a gap in the
// shared counter (43 packets a row, shorter than half its range) skips the rows lost.
class StripRows {
public:
    int place(int channel, int seq, int block, uint32_t ms);   // channel 0 .. 5, block 0 .. 13, ms of the day; the row (>= 0)
    void reset();
    double rowMs() const { return rowMs_; }                     // the time of a row seen so far (0: not yet)
private:
    int cur_ = -1, lastSeq_ = -1, lastPos_[kNumChannels] = {-1, -1, -1, -1, -1, -1};
    int64_t rowStartMs_ = -1;
    double rowMs_ = 0;
};

} // namespace dect2::meteor
