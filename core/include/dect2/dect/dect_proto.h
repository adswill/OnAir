// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DECT (cordless telephones, 1880 - 1900 MHz in Europe): the parts every base station broadcasts in the clear. Receive only.
// Ten carriers 1.728 MHz apart (carrier n at 1897.344 - 1.728 n MHz), 1.152 Mbit/s GFSK, a frame of 10 ms with 24 slots. A full slot starts
// with the S-field (16 preamble bits then a 16-bit sync word: 0xE98A from a base station (RFP), 0x1675 from a handset (PP)), then the
// 64-bit A-field: an 8-bit header (tail type TA 3 bits, Q1, BA 3 bits, Q2), 40 tail bits and a 16-bit CRC over the first 48. The B-field
// (voice or data, scrambled, and encrypted once a call has keys) is not decoded. EN 300 175-3.
#pragma once
#include <algorithm>
#include <cstdint>
#include <utility>
#include <string>

namespace dect {

constexpr double kBitRate = 1152000.0;
constexpr double kCarrier0Mhz = 1897.344, kSpacingMhz = 1.728;
constexpr int kCarriers = 10, kSlots = 24;
constexpr double kFrameSec = 0.010;
constexpr uint32_t kSyncRfp = 0xAAAAE98Au;   // preamble and sync word, first bit sent = most significant
constexpr uint32_t kSyncPp = 0x55551675u;

inline double carrierMhz(int n) { return kCarrier0Mhz - kSpacingMhz * n; }
inline int carrierOf(double mhz) {
    const double x = (kCarrier0Mhz - mhz) / kSpacingMhz;
    const int n = (int)(x + (x < 0 ? -0.5 : 0.5));
    return n >= 0 && n < kCarriers && (x - n < 0.2 && n - x < 0.2) ? n : -1;
}

// The regional bands. Every DECT carrier sits on one raster, 1.728 MHz apart (EN 300 175-2 clause 5.2 and annex F):
//   Europe       Fc = 1897.344 - c x 1.728 MHz, c = 0..9 (1880-1900 MHz)
//   other bands  Fc = F9 + c x 1.728 MHz, F9 = 1881.792, c = 10..63 (RF band 00001; the Europe raster continued upwards): US c = 23..27
//                (1921.536 - 1928.448), Brazil c = 17..21, Latin America c = 17..27
//   US ISM 900   Fc = 903.744 + (c - 10) x 1.728 MHz, c = 10..23 (RF band 01000, 902-928 MHz)
// A band here lists the carriers whose whole 1.728 MHz channel lies inside the allocation. Carrier n of a band counts from 0 at the band's
// first entry; the ETSI carrier number c is not used (it differs per RF band). Korea, Taiwan, Japan and China are not listed: no carrier plan
// for them is given in EN 300 175-2.
struct Band {
    const char* id;          // short code for settings
    const char* name;        // for the user
    const char* where;       // allocation and where it is used
    double firstMhz;         // centre of carrier 0
    int dir;                 // +1: carriers go up from carrier 0, -1: down (Europe counts down from 1897.344)
    int count;
    double loMhz, hiMhz;     // the allocation
};
constexpr int kBands = 5;
inline const Band& band(int b) {
    static const Band t[kBands] = {
        {"eu", "Europe 1880-1900 MHz", "Europe, most of Asia, Africa, Australia: the common DECT band", kCarrier0Mhz, -1, 10, 1880, 1900},
        {"us", "US / Canada 1920-1930 MHz (DECT 6.0)", "USA, Canada: UPCS band, DECT 6.0 phones", 1881.792 + 23 * kSpacingMhz, +1, 5, 1920, 1930},
        {"br", "Brazil 1910-1920 MHz", "Brazil", 1881.792 + 17 * kSpacingMhz, +1, 5, 1910, 1920},
        {"la", "Latin America 1910-1930 MHz", "Several Latin American countries", 1881.792 + 17 * kSpacingMhz, +1, 11, 1910, 1930},
        {"us900", "US ISM 902-928 MHz (DECT in ISM)", "USA, standard DECT in the 900 MHz ISM band: rare, most 900 MHz cordless phones are not DECT", 903.744, +1, 14, 902, 928},
    };
    return t[b < 0 ? 0 : b >= kBands ? kBands - 1 : b];
}
inline double carrierMhz(int b, int n) { const Band& x = band(b); return x.firstMhz + x.dir * kSpacingMhz * n; }
// the carrier of band b whose centre is within 0.2 carrier spacings of mhz (-1: none)
inline int carrierOf(int b, double mhz) {
    const Band& x = band(b);
    const double v = x.dir * (mhz - x.firstMhz) / kSpacingMhz;
    const int n = (int)(v + (v < 0 ? -0.5 : 0.5));
    return n >= 0 && n < x.count && (v - n < 0.2 && n - v < 0.2) ? n : -1;
}
// the band that has a carrier at mhz (-1: none); *carrier gets its number
inline int bandOf(double mhz, int* carrier = nullptr) {
    for (int b = 0; b < kBands; b++) { const int n = carrierOf(b, mhz); if (n >= 0) { if (carrier) *carrier = n; return b; } }
    return -1;
}
// the scan plan of a band: carrier centres in the order they are visited: ascending frequency
inline int scanPlan(int b, double* out, int cap) {
    const Band& x = band(b);
    int k = 0;
    for (int n = 0; n < x.count && k < cap; n++) out[k++] = carrierMhz(b, n);
    for (int i = 0; i < k; i++) for (int j = i + 1; j < k; j++) if (out[j] < out[i]) std::swap(out[i], out[j]);
    return k;
}
// lowest and highest centre a radio must reach to cover a band
inline double bandLowMhz(int b) { const Band& x = band(b); return std::min(carrierMhz(b, 0), carrierMhz(b, x.count - 1)); }
inline double bandHighMhz(int b) { const Band& x = band(b); return std::max(carrierMhz(b, 0), carrierMhz(b, x.count - 1)); }

// the A-field CRC: x^16 + x^10 + x^8 + x^7 + x^3 + 1 over the 48 bits (first bit sent = bit 47 of v), register starting at zero
inline uint16_t rcrc(uint64_t v48) {
    uint16_t reg = 0;
    for (int i = 47; i >= 0; i--) {
        const bool fb = ((reg >> 15) & 1) ^ ((v48 >> i) & 1);
        reg = (uint16_t)(reg << 1);
        if (fb) reg ^= 0x0589;
    }
    return reg;
}

enum TailType { TaCt0 = 0, TaCt1 = 1, TaNt = 2, TaNtCl = 3, TaQt = 4, TaEscape = 5, TaMt = 6, TaPt = 7 };
inline const char* tailName(int ta) {
    static const char* const n[8] = {"Ct (data 0)", "Ct (data 1)", "Nt (identity)", "Nt (connectionless)", "Qt (system info)", "escape", "Mt (MAC control)", "Pt (paging)"};
    return n[ta & 7];
}

struct AField {
    uint64_t raw = 0;        // the 64 bits, first bit sent = bit 63
    int ta = 0, q1 = 0, ba = 0, q2 = 0;
    uint64_t tail = 0;       // 40 bits
    uint16_t crc = 0;
    bool crcOk = false;      // the CRC agrees (as sent or complemented)
    bool crcInverted = false;   // ...and it was the complemented one
};

inline AField parseAField(uint64_t raw64) {
    AField a;
    a.raw = raw64;
    const uint64_t head48 = raw64 >> 16;
    a.ta = (int)(raw64 >> 61) & 7; a.q1 = (int)(raw64 >> 60) & 1; a.ba = (int)(raw64 >> 57) & 7; a.q2 = (int)(raw64 >> 56) & 1;
    a.tail = (raw64 >> 16) & 0xFFFFFFFFFFULL;
    a.crc = (uint16_t)(raw64 & 0xFFFF);
    const uint16_t c = rcrc(head48);
    a.crcOk = c == a.crc || (uint16_t)~c == a.crc;
    a.crcInverted = a.crcOk && c != a.crc;
    return a;
}

// builds an A-field with a correct CRC (tests and the generator); inverted: the complemented CRC
inline uint64_t makeAField(int ta, int q1, int ba, int q2, uint64_t tail40, bool inverted = false) {
    const uint64_t head = ((uint64_t)(ta & 7) << 5 | (uint64_t)(q1 & 1) << 4 | (uint64_t)(ba & 7) << 1 | (uint64_t)(q2 & 1)) & 0xFF;
    const uint64_t v48 = head << 40 | (tail40 & 0xFFFFFFFFFFULL);
    uint16_t c = rcrc(v48);
    if (inverted) c = (uint16_t)~c;
    return v48 << 16 | c;
}

// The base station identity (RFPI, 40 bits) carried by Nt: an exclusive-bit, the 3-bit class, then for class A the manufacturer code (EMC,
// 16 bits) and the fixed part number (17 bits). This split is read from the standard, not checked on a real station.
struct Rfpi {
    uint64_t value = 0;
    int arc = 0;             // access rights class
    int emc = -1;            // manufacturer code (class A)
    int fpn = -1;
};
inline Rfpi rfpiOf(uint64_t tail40) {
    Rfpi r;
    r.value = tail40;
    r.arc = (int)((tail40 >> 35) & 7);
    if (r.arc == 0) { r.emc = (int)((tail40 >> 19) & 0xFFFF); r.fpn = (int)((tail40 >> 2) & 0x1FFFF); }
    return r;
}
inline std::string hex40(uint64_t v) { char b[16]; snprintf(b, sizeof b, "%010llX", (unsigned long long)(v & 0xFFFFFFFFFFULL)); return b; }

}  // namespace dect
