// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Yaesu System Fusion (C4FM) air interface: frame layout and error coding, written from Yaesu's public "Amateur Radio Digital
// Specs" (revision 1.01), sections 3 and 4 and appendices A and B. No speech codec is here: voice goes to a user-installed plug-in.
//
// A frame is 100 ms = 960 bits = 480 dibits at 4800 symbols/s: frame sync (40 bit, D471C9634D), the FICH (200 bit) and a 720 bit payload.
// Dibit to symbol (Table 3-1): 00 = +1 (+900 Hz), 01 = +3 (+2700 Hz), 10 = -1 (-900 Hz), 11 = -3 (-2700 Hz).
//
// Not in the public specification: the bit order inside a 72 bit half-rate voice sub-frame (it is the AMBE+2 frame of DMR and NXDN: the positions agree with
// MMDVMHost's AMBEFEC tables, see fusion_rx.cpp) and inside a 144 bit full-rate one, the format of the GPS data, and the Golay generator that the spec prints
// (the standard (24,12,8) code with generator polynomial 0xC75 is used, see golay24()). Voice sub-frames are handed on as the bits were sent.
// The frame layout (FICH, DCH, Golay, convolution, interleave, whitening, CRC) is bit-identical to what MMDVMHost writes (tests/fusion/test_fusion_interop.cpp).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::fusion {

constexpr uint64_t kSyncWord = 0xD471C9634DULL;   // 40 bits
constexpr int kFrameBits = 960, kFrameDibits = 480, kSyncDibits = 20, kFichDibits = 100;

// Dibit levels
inline int dibitLevel(int d) { return d == 0 ? 1 : d == 1 ? 3 : d == 2 ? -1 : -3; }
inline int levelDibit(int lvl) { return lvl == 1 ? 0 : lvl == 3 ? 1 : lvl == -1 ? 2 : 3; }
void syncLevels(int8_t out[kSyncDibits]);   // the 20 symbol levels of the frame sync

// Building blocks (bits are one per byte, 0 or 1)
uint16_t crc16(const uint8_t* bits, int n);                       // x16+x12+x5+1, init 0, MSB first, result inverted
uint32_t golay24(uint16_t data12);                                // 24 bit codeword, data in the top 12 bits
bool golay24Decode(uint32_t word, uint16_t& data12, int& errors); // nearest codeword within 3 errors
std::vector<uint8_t> convEncode(const std::vector<uint8_t>& in);  // rate 1/2, K=5, G1=1+x3+x4, G2=1+x+x2+x4, output G1 G2 alternately
std::vector<uint8_t> convDecode(const std::vector<uint8_t>& coded, int& errors);   // hard decision Viterbi, the tail is part of the result
std::vector<uint8_t> interleaveDibits(const std::vector<uint8_t>& bits, int rowDibits);     // written in rows of rowDibits, read by columns
std::vector<uint8_t> deinterleaveDibits(const std::vector<uint8_t>& bits, int rowDibits);
void whiten(uint8_t* bits, int n);                                // PN(9,5), register 111001001, restarted per unit

struct Fich {
    int fi = 1;      // frame type: 0 header, 1 communication, 2 terminator, 3 test
    int cs = 2;      // callsign information (2: callsigns 1, 2 and 3)
    int cm = 0;      // call mode: 0 group / CQ, 3 individual
    int bn = 0, bt = 0;   // block number / total
    int fn = 0, ft = 0;   // frame number / total
    int mr = 0;      // message routing: 0 direct, 1 downlink (uplink idle), 2 downlink (uplink busy)
    int voip = 0;    // 0 local, 1 via internet
    int dt = 0;      // data type: 0 V/D mode 1, 1 data FR, 2 V/D mode 2, 3 voice FR
    int sqlOff = 0;  // 0 squelch code enabled, 1 disabled
    int sqlCode = 0; // 0 none, 1..126, 127 break-out
};
uint32_t fichPack(const Fich& f);
Fich fichUnpack(uint32_t w);
std::vector<uint8_t> fichEncode(const Fich& f);                    // 200 bits
bool fichDecode(const uint8_t* bits200, Fich& f, int& fixes);      // false when the CRC fails; fixes = bits the coding corrected

// A DCH unit: 160 data bits (20 bytes) in 360 coded bits, or 80 data bits (10 bytes) in 200 coded bits (V/D mode 2)
std::vector<uint8_t> dchEncode(const uint8_t* bytes, int nbytes);  // nbytes 20 -> 360 bits, 10 -> 200 bits
bool dchDecode(const uint8_t* bits, int nbits, uint8_t* bytes, int& fixes);

// V/D mode 2 voice: 49 bits -> 104 bits (72 in the VCH, 32 in the VeCH), and back by majority vote
std::vector<uint8_t> vd2VoiceEncode(const uint8_t* bits49);
void vd2VoiceDecode(const uint8_t* bits104, uint8_t* bits49);

// A whole frame
struct VoiceUnit {
    int kind = 0;                  // 1 V/D mode 1 channel frame (72 bit), 2 V/D mode 2 (49 bit voice, error-corrected), 3 voice FR channel frame (144 bit)
    std::vector<uint8_t> bytes;    // 9, 7 (49 bits, MSB first, 7 bits of padding) or 18 bytes
};
struct FrameContent {
    Fich fich;
    bool fichOk = false;
    int fichFixes = 0;
    std::vector<std::vector<uint8_t>> units;   // DCH units, in the order they are sent (20 or 10 bytes each)
    std::vector<bool> unitOk;                  // decode only: the unit's CRC passed
    std::vector<VoiceUnit> voice;
};
// Builds the 960 bits of a frame (sync included) from content.fich, the units and the voice for its data type; false when it does not fit
bool buildFrameBits(const FrameContent& c, std::vector<uint8_t>& bits960);
// Reads the 960 bits (the sync is not checked here). false when the FICH does not check.
bool parseFrameBits(const uint8_t* bits960, FrameContent& c);

// Callsign text helpers: 10 byte fields padded with spaces
std::string trimField(const uint8_t* p, size_t n);   // printable ASCII, trailing spaces removed, others as '.'
void padField(const std::string& s, uint8_t* p, size_t n);

} // namespace dect2::fusion
