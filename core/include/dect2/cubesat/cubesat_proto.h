// CubeSat telemetry framings, without any radio: the codes and frame layouts amateur satellites on 2 m / 70 cm use.
//  - AX.25 (packet_ax25.h) at 1200 baud AFSK or 9600 baud G3RUH: HDLC, NRZI, the G3RUH scrambler at 9600.
//  - GOMspace NanoCom AX100 "Mode 5" (ASM + Golay): GMSK / FSK, the sync word 0x930B51DE, a Golay (24,12) coded length field (low 8 bits
//    the length, 0x100 convolutional code, 0x200 scrambler, 0x400 Reed-Solomon), then the CCSDS randomised, Reed-Solomon (255,223) coded
//    CSP packet. As gr-satellites' AX100 ASM + Golay deframer.
//  - AX100 "Mode 6": a Reed-Solomon (255,223) coded CSP packet inside an HDLC frame, G3RUH scrambled and NRZI (no AX.25 header, no FCS).
//  - CCSDS: the sync word 0x1ACFFC1D, a CCSDS randomised Reed-Solomon (255,223) code block (dual basis, interleave 1), the frame length
//    fixed (255 bytes, or shortened); no convolutional code.
// Reed-Solomon (255,223): GF(2^8) x^8 + x^7 + x^2 + x + 1 (0x187), first root 112, root step 11, 32 parity bytes (as libfec's
// encode_rs_8 / decode_rs_8; the CCSDS block adds the dual basis of encode_rs_ccsds). Shortened codes are padded with zeros in front.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace cubesat {

enum Framing { Ax25Afsk1200 = 0, Ax25G3ruh, Ax100Mode5, Ax100Mode6, CcsdsRs, U482c, CcsdsConcat, Usp, Ngham, Mobitex, kNumFramings };   // the last five: cubesat_more.h (3.9)
const char* framingName(int f);        // "AX.25 AFSK 1200", ...
const char* framingShort(int f);       // "AFSK", "G3RUH", "AX100 M5", "AX100 M6", "CCSDS"

constexpr uint32_t kAsmAx100 = 0x930B51DEu;
constexpr uint32_t kAsmCcsds = 0x1ACFFC1Du;
constexpr int kRsParity = 32;
constexpr int kRsMaxData = 223;

// Reed-Solomon (255,223), conventional basis. rsParity: the 32 parity bytes of n <= 223 data bytes. rsDecode: block = data then parity
// (n = data + 32 bytes, n <= 255), corrected in place; the number of byte errors put right, or -1 when it cannot be decoded.
void rsParity(const uint8_t* data, size_t n, uint8_t* parity32);
int rsDecode(uint8_t* block, size_t n);
// the same in the CCSDS dual basis
void rsParityDual(const uint8_t* data, size_t n, uint8_t* parity32);
int rsDecodeDual(uint8_t* block, size_t n);

// Golay (24,12): the 12 data bits in the low half, the parity in the high half. Decode: the data, or -1 with more than three bit errors.
uint32_t golayEncode(uint32_t data12);
int golayDecode(uint32_t word24, int* errs = nullptr);

// XORs the CCSDS pseudo random sequence (x^8 + x^7 + x^5 + x^3 + 1, all ones at the start: FF 48 0E C0 ...) over n bytes
void ccsdsRandomize(uint8_t* d, size_t n);

// AX100 Mode 5: the length field's flags
constexpr int kAx100Viterbi = 0x100, kAx100Scrambler = 0x200, kAx100Rs = 0x400;

// The bytes on the air after the preamble: Mode 5 ASM + Golay field + scrambled RS block; CCSDS ASM + randomised RS block (frameLen bytes
// in all, 33 .. 255: data = frameLen - 32, zero padded); Mode 6 the RS block for HDLC.
std::vector<uint8_t> ax100Mode5Encode(const std::vector<uint8_t>& csp);
std::vector<uint8_t> ax100Mode6Encode(const std::vector<uint8_t>& csp);
std::vector<uint8_t> ccsdsEncode(const std::vector<uint8_t>& data, int frameLen = 255);

// Decoding what follows the sync word / what an HDLC frame held. rsErr: bytes corrected (-1: the RS code failed).
bool ax100Mode5Decode(uint8_t* body, size_t n, int flags, std::vector<uint8_t>& csp, int& rsErr);   // body: the bytes after the Golay field
bool ax100Mode6Decode(const std::vector<uint8_t>& hdlc, std::vector<uint8_t>& csp, int& rsErr);
bool ccsdsDecode(uint8_t* block, size_t n, std::vector<uint8_t>& data, int& rsErr);

// CSP (CubeSat Space Protocol) v1 header: 32 bits, big endian: priority 2, source 5, destination 5, destination port 6, source port 6,
// flags 8 (HMAC, XTEA, RDP, CRC32 in the low bits)
struct CspHeader { int prio = 0, src = 0, dst = 0, dport = 0, sport = 0, flags = 0; };
bool parseCsp(const std::vector<uint8_t>& p, CspHeader& h);
std::vector<uint8_t> buildCsp(const CspHeader& h, const std::vector<uint8_t>& payload);

// The satellites the mode names frames after (an editable table; the app keeps the user's in its settings)
struct SatDef {
    std::string name;
    int norad = 0;
    double mhz = 0;
    int framing = Ax25Afsk1200;
    int baud = 1200;
    std::string call;               // AX.25 source callsign without the SSID ("" none)
};
std::vector<SatDef> defaultSats();
std::string satsToText(const std::vector<SatDef>& v);          // name|norad|mhz|framing|baud|call;...
std::vector<SatDef> satsFromText(const std::string& s);

// a hex dump: "0000  01 02 ..  |..|" lines of 16 bytes
std::string hexDump(const std::vector<uint8_t>& d);
std::string asciiOf(const std::vector<uint8_t>& d);            // printable characters, others as '.'

} // namespace cubesat
} // namespace dect2
