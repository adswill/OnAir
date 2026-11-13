// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// BeiDou B1I navigation messages (BDS SIS ICD, CSNO): D1 for the MEO and IGSO satellites (50 bps with the 20 bit Neumann-Hoffman secondary code
// on the 1 ms code periods; subframes of 300 bits in 6 s) and D2 for the GEO satellites (500 bps without a secondary code; subframes of 0.6 s).
// Every subframe: 10 words of 30 bits. Word 1: the 11 bit preamble and 4 reserved bits sent as they are, then 11 bits protected by BCH(15,11,1);
// words 2..10: two BCH(15,11,1) blocks of 11 bits, interleaved bit by bit (ICD 5.1.3). The generator is g(X) = X^4 + X + 1.
//
// Bits are numbered as in the ICD figures, 1..300 over the subframe with the parity included: in word w (2..10) the 22 information bits are
// 30 (w - 1) + 1 .. 30 (w - 1) + 22 and the parity takes the last 8; in word 1 the information bits are 1..26.
#pragma once
#include "gnss_codes.h"
#include "gnss_nav.h"
#include <cstdint>

namespace dect2 {

constexpr unsigned kBdsPreamble = 0x712;                 // 11100010010 (ICD 5.2.4.1)
constexpr uint8_t kBdsNh[20] = {0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 0, 1, 0, 0, 1, 1, 1, 0};   // ICD 5.2.1
constexpr double kBdtGpsSeconds = 14.0;                  // BDT started at 2006-01-01 00:00:00 UTC, 14 s behind GPS time (ICD 3.3)
constexpr int kBdtGpsWeeks = 1356;                       // BDT week 0 began in GPS week 1356
constexpr double kBdsEarthRate = 7.2921150e-5;           // CGCS2000 (ICD 3.2)

// BCH(15,11,1): the 4 parity bits (the first sent in bit 3) of 11 information bits (the first sent in bit 10)
unsigned bch1511Parity(unsigned info11);
// Correct a received 15 bit block (the first bit in bit 14) in place; returns the number of bits corrected (0 or 1)
int bch1511Correct(unsigned* block15);

// The 300 bits as sent from the information bits (at their ICD positions; the parity positions are filled in)
void bdsEncodeSubframe(const uint8_t* info300, uint8_t* out300);
// The information bits from 300 received bits; returns the number of bits the BCH code corrected
int bdsDecodeSubframe(const uint8_t* rx300, uint8_t* info300);

// A field by its ICD bit position (1 based) and length, and fields in two or three parts (MSBs first)
uint32_t bdsField(const uint8_t* info300, int first, int len);
int32_t bdsSigned(uint32_t v, int len);
void bdsPut(uint8_t* info300, int first, int len, uint32_t v);
inline unsigned bdsFraId(const uint8_t* b) { return bdsField(b, 16, 3); }
inline unsigned bdsSow(const uint8_t* b) { return (bdsField(b, 19, 8) << 12) | bdsField(b, 31, 12); }

// ephemeris field identifiers in the D2 pages and the D1 subframes (raw integers)
enum BdsField { BfSatH1, BfAodc, BfUrai, BfWn, BfToc, BfTgd1, BfTgd2, BfAlpha0, BfAlpha1, BfAlpha2, BfAlpha3, BfBeta0, BfBeta1, BfBeta2, BfBeta3,
                BfA0, BfA1, BfA2, BfAode, BfDn, BfCuc, BfM0, BfCus, BfE, BfSqrtA, BfCic, BfCis, BfToe, BfI0, BfCrc, BfCrs, BfOmegaDot, BfOmega0, BfOmega, BfIdot,
                BfCount };
extern const int kBdsFieldBits[BfCount];

// The broadcast data of one satellite being collected
struct BdsNav {
    // D1 subframes 1..3, from the frame (SOW / 30) they came in
    GpsEphemeris pend;
    int64_t frameOf[4] = {-1, -1, -1, -1};
    uint32_t rawD1[BfCount] = {};    // the raw fields of subframes 1..3 as they arrive
    int wn = -1;                 // BDT week (13 bits)
    int aode = -1;
    // D2: the 72 data bits after the page number of pages 1..10 of subframe 1, and the SOW they came with
    uint8_t d2[11][72] = {};
    int64_t d2Sow[11] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    // D1 subframe 5 page 9: BDT - GPS (A0GPS, A1GPS)
    bool gpsValid = false;
    double a0gps = 0, a1gps = 0;
    int parts() const;           // ephemeris parts collected: subframes 1..3 (D1), pages 1..10 scaled to 3 (D2)
};

// Fold a decoded subframe into the collection. `geo`: a D2 message. Returns true when it completed an ephemeris (in *eph, with its BDT week in *week).
bool bdsHandleSubframe(BdsNav& n, const uint8_t* info300, bool geo, int prn, GpsEphemeris* eph, int* week);

// D2 subframe 1 page layout: the 72 data bits of page p (1..10) after the page number, from an ephemeris (for the generator) and the order of the fields.
// Exposed so the generator and the receiver use the ICD figures from one place: Figure 5-14-1 .. 5-14-10.
struct BdsD2Field { int id, bits; };     // id -1: reserved
const BdsD2Field* bdsD2Page(int page, int* n);

// raw integers to the ephemeris (scale factors of ICD Tables 5-7 and 5-10)
void bdsEphemerisFromRaw(const uint32_t* raw, int prn, GpsEphemeris& e);

} // namespace dect2
