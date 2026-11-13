// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// APCO Project 25 Phase 1 (TIA-102.BAAA) protocol pieces shared by the receiver and the test signal: frame sync, NID with BCH(63,16),
// status symbols, the Reed-Solomon, Hamming and Golay codes of the header / link control / encryption sync, the rate 1/2 trellis code
// and interleave of TSBKs and PDU headers, TSBK and link control fields, and the IMBE voice frame interleave.
// Receive only: nothing here is used to transmit.
#pragma once
#include <cstdint>
#include <string>

namespace dect2 {
namespace p25 {

constexpr int kSyncSym = 24;
extern const int kSync[kSyncSym];          // the 48-bit frame sync 0x5575F5FF77FF as C4FM levels (+3 / -3)
constexpr int kNidSym = 32;
constexpr int kStatusEvery = 36;           // every 36th dibit of a frame (positions 35, 71, ...) is a status symbol
inline bool isStatusPos(int p) { return p % kStatusEvery == kStatusEvery - 1; }

// data units
enum Duid { kHdu = 0, kTdu = 3, kLdu1 = 5, kTsbk = 7, kLdu2 = 10, kPdu = 12, kTdulc = 15 };
const char* duidName(int duid);
int frameLen(int duid);                    // the frame length in dibits (status symbols included); TSBK / PDU: up to three blocks

// C4FM dibit <-> level: 01 +3, 00 +1, 10 -1, 11 -3
inline int dibitToSym(int d) { static const int t[4] = {1, 3, -1, -3}; return t[d & 3]; }
inline int symToDibit(int s) { return s == 3 ? 1 : s == 1 ? 0 : s == -1 ? 2 : 3; }

// NID: NAC (12 bits) and DUID (4 bits), BCH(63,16,23) plus a parity bit. bits[0] is sent first.
void nidEncode(int nac, int duid, uint8_t bits[64]);
// With the site's NAC known only the frame types remain: the nearest of those code words, up to 16 bit errors and clearly nearer than the runner-up (the BCH code alone stops at 11)
bool nidDecodeKnownNac(const uint8_t bits[64], int nac, int& duid, int* errs = nullptr);
bool nidDecode(const uint8_t bits[64], int& nac, int& duid, int* errs = nullptr);   // corrects up to 11 bit errors (accepts at most 10)

// Reed-Solomon over GF(64) (x^6 + x + 1), shortened from (63, ...): sym[0] is the first hexbit sent, data first, then n - k parity
void rsEncode(uint8_t* sym, int n, int k);
bool rsDecode(uint8_t* sym, int n, int k, int* errs = nullptr);

// Hamming(10,6,3) of the LDU hexbits; Golay(24,12,8) and its shortened (18,6,8) of the header
uint16_t hamming10Encode(int hex);         // 10 bits, the hexbit first (bit 9 sent first)
int hamming10Decode(uint16_t cw, int* errs = nullptr);   // the hexbit (one error corrected)
uint32_t golay24Encode(int d12);           // 24 bits, the data first (bit 23 sent first)
int golay24Decode(uint32_t cw, int* errs = nullptr);     // -1 when more than three errors
uint32_t golay18Encode(int hex);           // 18 bits
int golay18Decode(uint32_t cw, int* errs = nullptr);

// CRC-CCITT of TSBKs and PDU headers (80 bits of data, complemented)
uint16_t crcCcitt(const uint8_t* bits, int n);

// Rate 1/2 trellis code + the 98-dibit interleave: 96 bits <-> 98 dibits as sent
void trellisEncode(const uint8_t bits[96], uint8_t dibits[98]);
bool trellisDecode(const float lv[98], uint8_t bits[96], float* metric = nullptr);   // lv: the received levels (about -3..3), as sent

// One TSBK: last block, protected, opcode, manufacturer, 64 bits of arguments (bit 63 sent first)
struct Tsbk {
    bool last = true, prot = false;
    int opcode = 0, mfid = 0;
    uint64_t args = 0;
};
void tsbkBits(const Tsbk& t, uint8_t bits[96]);   // with its CRC
bool tsbkParse(const uint8_t bits[96], Tsbk& t);  // CRC checked
enum Opcode { kGrpVGrant = 0x00, kGrpVGrantUpd = 0x02, kUuVGrant = 0x04, kIdenUpVu = 0x34, kRfssSts = 0x3A, kNetSts = 0x3B, kAdjSts = 0x3C, kIdenUp = 0x3D };
const char* opcodeName(int op);

// Link control (72 bits, LDU1 and TDULC)
struct Lc {
    int lco = 0, mfid = 0;
    bool prot = false;
    int svcOpts = 0;          // 0x80 emergency, 0x40 encrypted
    int tg = -1, src = -1, dst = -1;
    uint64_t raw = 0;         // the 72 bits' low 64 (for others)
};
void lcHexbits(const Lc& l, uint8_t hex[12]);
bool lcParse(const uint8_t hex[12], Lc& l);
enum Lco { kLcGroup = 0x00, kLcUnit = 0x03, kLcTerm = 0x0F };

// Encryption sync (LDU2) and the header (HDU)
struct Crypto {
    uint8_t mi[9] = {};
    int algId = 0x80, keyId = 0;
    int mfid = 0, tg = -1;    // the header only
};
constexpr int kAlgClear = 0x80;
const char* algName(int alg);
void esHexbits(const Crypto& c, uint8_t hex[16]);
void esParse(const uint8_t hex[16], Crypto& c);
void hduHexbits(const Crypto& c, uint8_t hex[20]);
void hduParse(const uint8_t hex[20], Crypto& c);

// IMBE: one voice frame is 72 dibits as sent; fr is c0..c7 (23, 23, 23, 23, 15, 15, 15, 7 bits) as mbelib takes it
void imbeDeinterleave(const uint8_t dibits[72], char fr[8][23]);
void imbeInterleave(const char fr[8][23], uint8_t dibits[72]);
void imbeFlatten(const char fr[8][23], uint8_t bits[144]);   // c0 first
void imbeUnflatten(const uint8_t bits[144], char fr[8][23]);

// a channel number (4-bit identifier, 12-bit channel) through an identifier update
struct Iden {
    bool valid = false, vu = false;
    double baseHz = 0, spacingHz = 0, txOffsetHz = 0, bwHz = 0;
};
inline double channelHz(const Iden& i, int ch) { return i.valid ? i.baseHz + (ch & 0xFFF) * i.spacingHz : 0; }
std::string channelText(int ch);   // "1-0x0A5"

} // namespace p25
} // namespace dect2
