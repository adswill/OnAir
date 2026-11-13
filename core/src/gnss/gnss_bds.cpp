// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// BeiDou B1I navigation messages (see gnss_bds.h). Field positions: BDS SIS ICD Figures 5-8 .. 5-11 (D1) and 5-14 (D2), scale factors Tables 5-7, 5-10, 5-17.
#include "dect2/gnss/gnss_bds.h"
#include "dect2/common/diag.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {

// ------------------------------------------------------------------ BCH(15,11,1)
unsigned bch1511Parity(unsigned info11) {
    // the remainder of info(X) X^4 divided by X^4 + X + 1
    unsigned r = (info11 & 0x7FF) << 4;
    for (int b = 14; b >= 4; b--) if (r & (1u << b)) r ^= 0x13u << (b - 4);
    return r & 0xF;
}

int bch1511Correct(unsigned* block15) {
    unsigned r = *block15 & 0x7FFF;
    for (int b = 14; b >= 4; b--) if (r & (1u << b)) r ^= 0x13u << (b - 4);
    const unsigned syn = r & 0xF;
    if (syn == 0) return 0;
    // a single error at X^k leaves the remainder of X^k: the ROM table of ICD Table 5-2
    unsigned x = 1;
    for (int k = 0; k < 15; k++) {
        if (x == syn) { *block15 ^= 1u << k; return 1; }
        x <<= 1;
        if (x & 0x10) x ^= 0x13;
    }
    return 0;
}

// ------------------------------------------------------------------ subframes
uint32_t bdsField(const uint8_t* b, int first, int len) {
    uint32_t v = 0;
    for (int k = 0; k < len; k++) v = (v << 1) | (b[first - 1 + k] & 1u);
    return v;
}
int32_t bdsSigned(uint32_t v, int len) {
    if (len < 32 && (v & (1u << (len - 1)))) return (int32_t)(v | ~((1u << len) - 1));
    return (int32_t)v;
}
void bdsPut(uint8_t* b, int first, int len, uint32_t v) {
    for (int k = 0; k < len; k++) b[first - 1 + k] = (uint8_t)((v >> (len - 1 - k)) & 1u);
}

void bdsEncodeSubframe(const uint8_t* info, uint8_t* out) {
    // word 1: bits 1..15 as they are, 16..26 and their parity
    std::memcpy(out, info, 26);
    {
        const unsigned p = bch1511Parity(bdsField(info, 16, 11));
        for (int k = 0; k < 4; k++) out[26 + k] = (uint8_t)((p >> (3 - k)) & 1u);
    }
    for (int w = 1; w < 10; w++) {
        const int s = 30 * w + 1;
        const unsigned x1 = bdsField(info, s, 11), x2 = bdsField(info, s + 11, 11);
        const unsigned c1 = (x1 << 4) | bch1511Parity(x1), c2 = (x2 << 4) | bch1511Parity(x2);
        // X1^1 X2^1 X1^2 X2^2 .. X1^11 X2^11 P1^1 P2^1 .. P1^4 P2^4
        for (int j = 0; j < 15; j++) {
            out[30 * w + 2 * j] = (uint8_t)((c1 >> (14 - j)) & 1u);
            out[30 * w + 2 * j + 1] = (uint8_t)((c2 >> (14 - j)) & 1u);
        }
    }
}

int bdsDecodeSubframe(const uint8_t* rx, uint8_t* info) {
    int fixed = 0;
    std::memcpy(info, rx, 15);
    {
        unsigned c = 0;
        for (int k = 15; k < 30; k++) c = (c << 1) | (rx[k] & 1u);
        fixed += bch1511Correct(&c);
        for (int k = 0; k < 15; k++) info[15 + k] = (uint8_t)((c >> (14 - k)) & 1u);
    }
    for (int w = 1; w < 10; w++) {
        unsigned c1 = 0, c2 = 0;
        for (int j = 0; j < 15; j++) { c1 = (c1 << 1) | (rx[30 * w + 2 * j] & 1u); c2 = (c2 << 1) | (rx[30 * w + 2 * j + 1] & 1u); }
        fixed += bch1511Correct(&c1) + bch1511Correct(&c2);
        uint8_t* o = info + 30 * w;
        for (int j = 0; j < 11; j++) { o[j] = (uint8_t)((c1 >> (14 - j)) & 1u); o[11 + j] = (uint8_t)((c2 >> (14 - j)) & 1u); }
        for (int j = 0; j < 4; j++) { o[22 + j] = (uint8_t)((c1 >> (3 - j)) & 1u); o[26 + j] = (uint8_t)((c2 >> (3 - j)) & 1u); }
    }
    return fixed;
}

// ------------------------------------------------------------------ fields
const int kBdsFieldBits[BfCount] = {1, 5, 4, 13, 17, 10, 10, 8, 8, 8, 8, 8, 8, 8, 8, 24, 22, 11, 5, 16, 18, 32, 18, 32, 32, 18, 18, 17, 32, 18, 18, 24, 32, 32, 14};

namespace {
struct Piece { int sf, field, pos, len; };
// D1 subframes 1..3, every field in its parts, MSBs first (Figures 5-8, 5-9, 5-10)
const Piece kD1[] = {
    {1, BfSatH1, 43, 1}, {1, BfAodc, 44, 5}, {1, BfUrai, 49, 4}, {1, BfWn, 61, 13}, {1, BfToc, 74, 9}, {1, BfToc, 91, 8}, {1, BfTgd1, 99, 10},
    {1, BfTgd2, 109, 4}, {1, BfTgd2, 121, 6}, {1, BfAlpha0, 127, 8}, {1, BfAlpha1, 135, 8}, {1, BfAlpha2, 151, 8}, {1, BfAlpha3, 159, 8},
    {1, BfBeta0, 167, 6}, {1, BfBeta0, 181, 2}, {1, BfBeta1, 183, 8}, {1, BfBeta2, 191, 8}, {1, BfBeta3, 199, 4}, {1, BfBeta3, 211, 4},
    {1, BfA2, 215, 11}, {1, BfA0, 226, 7}, {1, BfA0, 241, 17}, {1, BfA1, 258, 5}, {1, BfA1, 271, 17}, {1, BfAode, 288, 5},
    {2, BfDn, 43, 10}, {2, BfDn, 61, 6}, {2, BfCuc, 67, 16}, {2, BfCuc, 91, 2}, {2, BfM0, 93, 20}, {2, BfM0, 121, 12}, {2, BfE, 133, 10}, {2, BfE, 151, 22},
    {2, BfCus, 181, 18}, {2, BfCrc, 199, 4}, {2, BfCrc, 211, 14}, {2, BfCrs, 225, 8}, {2, BfCrs, 241, 10}, {2, BfSqrtA, 251, 12}, {2, BfSqrtA, 271, 20},
    {2, BfToe, 291, 2},
    {3, BfToe, 43, 10}, {3, BfToe, 61, 5}, {3, BfI0, 66, 17}, {3, BfI0, 91, 15}, {3, BfCic, 106, 7}, {3, BfCic, 121, 11}, {3, BfOmegaDot, 132, 11},
    {3, BfOmegaDot, 151, 13}, {3, BfCis, 164, 9}, {3, BfCis, 181, 9}, {3, BfIdot, 190, 13}, {3, BfIdot, 211, 1}, {3, BfOmega0, 212, 21},
    {3, BfOmega0, 241, 11}, {3, BfOmega, 252, 11}, {3, BfOmega, 271, 21},
};
// D2 subframe 1 pages 1..10 (Figures 5-14-1 .. 5-14-10)
const BdsD2Field kP1[] = {{BfSatH1, 1}, {BfAodc, 5}, {BfUrai, 4}, {BfWn, 13}, {BfToc, 17}, {BfTgd1, 10}, {BfTgd2, 10}, {-1, 12}};
const BdsD2Field kP2[] = {{BfAlpha0, 8}, {BfAlpha1, 8}, {BfAlpha2, 8}, {BfAlpha3, 8}, {BfBeta0, 8}, {BfBeta1, 8}, {BfBeta2, 8}, {BfBeta3, 8}, {-1, 8}};
const BdsD2Field kP3[] = {{-1, 6}, {-1, 22}, {-1, 10}, {BfA0, 24}, {BfA1, 4}, {-1, 6}};
const BdsD2Field kP4[] = {{BfA1, 18}, {BfA2, 11}, {BfAode, 5}, {BfDn, 16}, {BfCuc, 14}, {-1, 8}};
const BdsD2Field kP5[] = {{BfCuc, 4}, {BfM0, 32}, {BfCus, 18}, {BfE, 10}, {-1, 8}};
const BdsD2Field kP6[] = {{BfE, 22}, {BfSqrtA, 32}, {BfCic, 10}, {-1, 8}};
const BdsD2Field kP7[] = {{BfCic, 8}, {BfCis, 18}, {BfToe, 17}, {BfI0, 21}, {-1, 8}};
const BdsD2Field kP8[] = {{BfI0, 11}, {BfCrc, 18}, {BfCrs, 18}, {BfOmegaDot, 19}, {-1, 6}};
const BdsD2Field kP9[] = {{BfOmegaDot, 5}, {BfOmega0, 32}, {BfOmega, 27}, {-1, 8}};
const BdsD2Field kP10[] = {{BfOmega, 5}, {BfIdot, 14}, {-1, 53}};
} // namespace

const BdsD2Field* bdsD2Page(int page, int* n) {
    static const BdsD2Field* pages[11] = {nullptr, kP1, kP2, kP3, kP4, kP5, kP6, kP7, kP8, kP9, kP10};
    static const int counts[11] = {0, 8, 9, 6, 6, 5, 4, 5, 5, 4, 3};
    if (page < 1 || page > 10) { *n = 0; return nullptr; }
    *n = counts[page];
    return pages[page];
}

void bdsEphemerisFromRaw(const uint32_t* r, int prn, GpsEphemeris& e) {
    auto s = [&](int f) { return (double)bdsSigned(r[f], kBdsFieldBits[f]); };
    e = GpsEphemeris();
    e.prn = prn; e.beidou = true; e.geo = bdsIsGeo(prn);
    e.health = (int)r[BfSatH1];
    e.uraIndex = (int)r[BfUrai];
    e.wn = (int)r[BfWn];
    e.toc = r[BfToc] * 8.0;
    e.tgd = s(BfTgd1) * 0.1e-9;
    e.af0 = s(BfA0) * std::ldexp(1.0, -33);
    e.af1 = s(BfA1) * std::ldexp(1.0, -50);
    e.af2 = s(BfA2) * std::ldexp(1.0, -66);
    e.iodc = e.iode2 = e.iode3 = (int)r[BfAode];
    e.toe = r[BfToe] * 8.0;
    e.sqrtA = r[BfSqrtA] * std::ldexp(1.0, -19);
    e.e = r[BfE] * std::ldexp(1.0, -33);
    e.omega = s(BfOmega) * std::ldexp(1.0, -31) * kPi;
    e.dn = s(BfDn) * std::ldexp(1.0, -43) * kPi;
    e.m0 = s(BfM0) * std::ldexp(1.0, -31) * kPi;
    e.omega0 = s(BfOmega0) * std::ldexp(1.0, -31) * kPi;
    e.omegaDot = s(BfOmegaDot) * std::ldexp(1.0, -43) * kPi;
    e.i0 = s(BfI0) * std::ldexp(1.0, -31) * kPi;
    e.idot = s(BfIdot) * std::ldexp(1.0, -43) * kPi;
    e.cuc = s(BfCuc) * std::ldexp(1.0, -31);
    e.cus = s(BfCus) * std::ldexp(1.0, -31);
    e.crc = s(BfCrc) * std::ldexp(1.0, -6);
    e.crs = s(BfCrs) * std::ldexp(1.0, -6);
    e.cic = s(BfCic) * std::ldexp(1.0, -31);
    e.cis = s(BfCis) * std::ldexp(1.0, -31);
    e.has1 = e.has2 = e.has3 = true;
}

int BdsNav::parts() const {
    int n = 0;
    for (int k = 1; k <= 3; k++) n += frameOf[k] >= 0 && frameOf[k] == frameOf[1];
    int pages = 0;
    for (int p = 1; p <= 10; p++) pages += d2Sow[p] >= 0;
    return std::max(n, pages * 3 / 10);
}

bool bdsHandleSubframe(BdsNav& n, const uint8_t* b, bool geo, int prn, GpsEphemeris* eph, int* week) {
    const unsigned id = bdsFraId(b);
    const int64_t sow = bdsSow(b);
    if (!geo) {
        if (id == 5 && bdsField(b, 44, 7) == 9) {
            // page 9 of subframe 5: A0GPS (14 bits, 0.1 ns) and A1GPS (16 bits, 0.1 ns/s) (Figure 5-11-4, Table 5-17); zero while not broadcast
            const uint32_t a0 = bdsField(b, 97, 14), a1 = (bdsField(b, 111, 2) << 14) | bdsField(b, 121, 14);
            n.gpsValid = a0 != 0 || a1 != 0;
            n.a0gps = bdsSigned(a0, 14) * 0.1e-9;
            n.a1gps = bdsSigned(a1, 16) * 0.1e-9;
            return false;
        }
        if (id < 1 || id > 3) return false;
        n.frameOf[id] = sow / 30;
        // the subframe's own fields; a field split over subframes 2 and 3 (toe) is put together once both are there
        uint32_t* r = n.rawD1;
        for (const Piece& p : kD1) {
            if (p.sf != (int)id) continue;
            int before = 0;      // bits of this field in earlier pieces
            for (const Piece& q : kD1) { if (&q == &p) break; if (q.field == p.field) before += q.len; }
            const int total = kBdsFieldBits[p.field];
            const int shift = total - before - p.len;
            const uint32_t mask = (p.len >= 32 ? 0xFFFFFFFFu : ((1u << p.len) - 1)) << shift;
            r[p.field] = (r[p.field] & ~mask) | ((bdsField(b, p.pos, p.len) << shift) & mask);
        }
        if (n.frameOf[1] < 0 || n.frameOf[1] != n.frameOf[2] || n.frameOf[1] != n.frameOf[3]) return false;
        bdsEphemerisFromRaw(r, prn, *eph);
        *week = (int)r[BfWn];
        return true;
    }
    // D2: the pages of subframe 1
    if (id != 1) return false;
    const int page = (int)bdsField(b, 43, 4);
    if (page < 1 || page > 10) {
        DIAG_DEBUG_EVERY(936769, 5.0, "A BeiDou D2 subframe 1 on PRN %d carries page number %d, outside 1 to 10, so the page is dropped.", prn, page);
        return false;
    }
    static const int kPos[4][2] = {{47, 6}, {61, 22}, {91, 22}, {121, 22}};
    int k = 0;
    for (auto& pr : kPos) for (int j = 0; j < pr[1]; j++) n.d2[page][k++] = b[pr[0] - 1 + j];
    n.d2Sow[page] = sow;
    // all ten pages within one 30 s cycle
    for (int p = 1; p <= 10; p++) if (n.d2Sow[p] < 0 || n.d2Sow[p] > sow || sow - n.d2Sow[p] >= 30) return false;
    uint32_t r[BfCount] = {};
    int got[BfCount] = {};
    for (int p = 1; p <= 10; p++) {
        int cnt = 0;
        const BdsD2Field* fl = bdsD2Page(p, &cnt);
        int at = 0;
        for (int f = 0; f < cnt; f++) {
            if (fl[f].id >= 0) {
                for (int j = 0; j < fl[f].bits; j++) r[fl[f].id] = (r[fl[f].id] << 1) | n.d2[p][at + j];
                got[fl[f].id] += fl[f].bits;
            }
            at += fl[f].bits;
        }
    }
    for (int f = 0; f < BfCount; f++) if (got[f] != kBdsFieldBits[f]) {
        DIAG_DEBUG_EVERY(815174, 5.0, "The ten BeiDou D2 pages on PRN %d gave %d bits for ephemeris field %d where %d are needed, so no ephemeris is built from them.", prn, got[f], f, kBdsFieldBits[f]);
        return false;
    }
    bdsEphemerisFromRaw(r, prn, *eph);
    *week = (int)r[BfWn];
    // the next ephemeris needs ten new pages
    for (int p = 1; p <= 10; p++) n.d2Sow[p] = -1;
    return true;
}

} // namespace dect2
