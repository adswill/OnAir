// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// JS8 (JS8Call) protocol: the 87-bit frame (72 message bits, 3 transmission flags, CRC-12), the (174,87) LDPC code, the 79 8-FSK tones
// with three 7x7 Costas arrays, the submodes, and the message layer (heartbeats / CQ, directed messages and commands, compound calls,
// Huffman coded free text).
//
// Sources (read, re-implemented, credited): JS8Call (J. Sherer KN4CRD et al., GPL-3): JS8.cpp (encode, the CRC-12, the parity matrix,
// the belief propagation tables), varicode.cpp / varicode.h (frame types, callsign, grid and command packing, the Huffman table),
// lib/js8/genjs8.f90 and js8{a,b,c}_params.f90 (Costas arrays, symbol lengths, slot lengths, start delays); WSJT-X 1.x (K1JT et al.,
// GPL-3) lib/ft8/ldpc_174_87_params.f90, whose (174,87) code JS8 kept from the first FT8. The grid locator to latitude / longitude
// conversion is the FT8 one (hfdig_ftx_msg.cpp).
//
// Not decoded: JSC compressed text (a dictionary of some 260 000 words that OnAir does not carry); such frames show "[compressed]".
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace js8 {

// ---------------------------------------------------------------- the code
constexpr int kN = 174, kK = 87, kM = 87;
extern const char* const kParityHex[kM];     // generator rows: 87 message bits each (22 hex digits, the last bit unused)
extern const uint8_t kNmCount[kM];
extern const uint8_t kNm[kM][7];             // the codeword bits (0-origin) of each parity check

uint16_t crc12(const uint8_t bytes[11]);     // augmented CRC-12 (polynomial 0xc06) of the 11 bytes, xor 42
// 72 message bits + 3 flag bits -> the 87 bits with the CRC
void frameBits(const uint8_t* msg72, int flags, uint8_t* bits87);
bool checkCrc(const uint8_t* bits87);
// 87 bits -> the 174-bit codeword: 87 parity bits, then the 87 bits
void encode174(const uint8_t* bits87, uint8_t* cw174);
// Belief propagation. llr > 0 means 1. Returns the parity checks still wrong (0 = a codeword).
int ldpcDecode(const float* llr, int iters, uint8_t* cw174);

// ---------------------------------------------------------------- the waveform
enum Submode { kNormal = 0, kFast = 1, kTurbo = 2, kSubmodes = 3 };
struct Spec {
    const char* name;
    int sps12k;          // samples per symbol at 12 kHz (1920, 1200, 600)
    double period;       // slot, s (15, 10, 6)
    double startSec;     // the transmission starts this long after the slot (0.5, 0.2, 0.1)
    double baud() const { return 12000.0 / sps12k; }   // = the tone spacing (6.25, 10, 20 Hz)
    double durSec() const { return 79.0 / baud(); }
};
const Spec& spec(int submode);
const uint8_t* costas(int submode, int which);   // the Costas array (0, 1, 2) of the submode: normal the original one three times
constexpr int kSymbols = 79;
// 87 bits -> 79 tones (0 .. 7): Costas, 29 parity symbols, Costas, 29 data symbols, Costas; 3 bits per tone, MSB first, no Gray code
std::vector<int> tones(int submode, const uint8_t* bits87);
inline bool isCostasSymbol(int i) { return i < 7 || (i >= 36 && i < 43) || i >= 72; }
// the data symbols (58) in codeword order
inline int dataSymbol(int j) { return j < 29 ? 7 + j : 43 + (j - 29); }

// ---------------------------------------------------------------- the message layer
enum FrameType { kHeartbeat = 0, kCompound = 1, kCompoundDirected = 2, kDirected = 3, kData = 4 };
enum Flags { kFirst = 1, kLast = 2, kFastData = 4 };   // the 3 transmission flag bits

struct Frame {
    int type = -1;               // FrameType, -1 not understood
    int flags = 0;
    std::string from, to;        // callsigns (to may be a group, "@ALLCALL")
    std::string cmd;             // directed command (" SNR", " GRID", ">", " " = free text follows ...), as JS8Call writes it
    int cmdCode = -1;
    std::string num;             // the number of the command (an SNR: "+05", "-12")
    std::string grid;            // 4-character locator (heartbeat, CQ, compound)
    bool cq = false;             // a heartbeat frame that is a CQ
    std::string cqText;          // "CQ CQ CQ", "CQ DX" ...
    std::string text;            // data frames: the free text
    bool compressed = false;     // data in JSC compression (not decoded)
    std::string display() const; // the frame as JS8Call shows it ("LY2ABC: @HB HEARTBEAT KO24", "OK1XX: LY2ABC SNR -12", text ...)
};

// 72 bits <-> 12 characters of the frame alphabet "0-9A-Za-z-+" (JS8Call's on-air text of a frame)
std::string bitsToChars(const uint8_t* msg72);
bool charsToBits(const std::string& twelve, uint8_t* msg72);
Frame unpackFrame(const uint8_t* msg72, int flags);

// packers (the test signals): false when the text cannot be sent so
bool packHeartbeat(const std::string& call, const std::string& grid, uint8_t* msg72);
bool packCq(const std::string& call, const std::string& grid, int cqIndex, uint8_t* msg72);   // cqIndex: 0 "CQ CQ CQ" .. 7 "CQ"
bool packDirected(const std::string& from, const std::string& to, int cmdCode, int num, uint8_t* msg72);   // num: SNR for 25 / 29 (else ignored)
// Huffman coded free text: as much of text as fits; returns the characters taken (0: none)
int packData(const std::string& text, uint8_t* msg72);
// A whole message as JS8Call would send it ("FROM: TO TEXT" = a directed frame with the free text command and data frames;
// cmdCode >= 0 and empty text = one directed frame): the frames' 72 bits and flags
struct TxFrame { uint8_t bits[72]; int flags; };
std::vector<TxFrame> buildMessage(const std::string& from, const std::string& to, int cmdCode, int num, const std::string& text);

int commandCode(const std::string& cmd);     // " SNR" -> 25, ">" -> 5 ...; -1 unknown
const char* commandText(int code);           // the inverse ("" unknown)

// callsign <-> 28 bits (6-character callsigns, /P, the @ groups); 0 when it cannot be packed
uint32_t packCallsign(const std::string& call, bool* portable);
std::string unpackCallsign(uint32_t v, bool portable);
uint64_t packAlphaNumeric50(const std::string& v);
std::string unpackAlphaNumeric50(uint64_t v);
uint16_t packGrid(const std::string& grid4);   // 32767 none
std::string unpackGrid(uint16_t v);            // "" none
bool gridToLatLon(const std::string& grid, double& lat, double& lon);   // the FT8 conversion (north / east positive)

} // namespace js8
} // namespace dect2
