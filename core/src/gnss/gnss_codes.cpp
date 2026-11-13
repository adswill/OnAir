// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Spreading codes of the satellite navigation signals (see gnss_codes.h).
#include "dect2/gnss/gnss_codes.h"
#include "dect2/gnss/gnss_tel.h"
#include <cstring>

namespace dect2 {

// G2 output stages (1 based) per PRN, IS-GPS-200 Table 3-I "code phase selection"
static const uint8_t kCaTaps[33][2] = {
    {0, 0}, {2, 6}, {3, 7}, {4, 8}, {5, 9}, {1, 9}, {2, 10}, {1, 8}, {2, 9}, {3, 10}, {2, 3}, {3, 4}, {5, 6}, {6, 7}, {7, 8}, {8, 9}, {9, 10},
    {1, 4}, {2, 5}, {3, 6}, {4, 7}, {5, 8}, {6, 9}, {1, 3}, {4, 6}, {5, 7}, {6, 8}, {7, 9}, {8, 10}, {1, 6}, {2, 7}, {3, 8}, {4, 9}};

bool gpsCaChips(int prn, uint8_t* chips) {
    if (prn < 1 || prn > 32) return false;
    // G1 = 1 + x^3 + x^10 (feedback from stages 3 and 10), G2 = 1 + x^2 + x^3 + x^6 + x^8 + x^9 + x^10 (stages 2, 3, 6, 8, 9, 10); both start all ones;
    // the output is G1 stage 10 xor the two chosen G2 stages
    int g1[11], g2[11];
    for (int i = 1; i <= 10; i++) g1[i] = g2[i] = 1;
    const int a = kCaTaps[prn][0], b = kCaTaps[prn][1];
    for (int n = 0; n < kGpsCaLen; n++) {
        chips[n] = (uint8_t)(g1[10] ^ g2[a] ^ g2[b]);
        const int f1 = g1[3] ^ g1[10];
        const int f2 = g2[2] ^ g2[3] ^ g2[6] ^ g2[8] ^ g2[9] ^ g2[10];
        for (int i = 10; i > 1; i--) { g1[i] = g1[i - 1]; g2[i] = g2[i - 1]; }
        g1[1] = f1; g2[1] = f2;
    }
    return true;
}

unsigned gpsCaFirst10(int prn) {
    uint8_t c[kGpsCaLen];
    if (!gpsCaChips(prn, c)) return 0;
    unsigned v = 0;
    for (int i = 0; i < 10; i++) v = (v << 1) | c[i];
    return v;
}

// G2 delays in chips of the SBAS PRNs 120..158 (RTCA DO-229) and the QZSS PRNs 193..202 (IS-QZSS-PNT), from the gps.gov L1 C/A PRN code assignments
static const int16_t kSbasDelay[39] = {145, 175, 52, 21, 237, 235, 886, 657, 634, 762, 355, 1012, 176, 603, 130, 359, 595, 68, 386, 797,
                                       456, 499, 883, 307, 127, 211, 121, 118, 163, 628, 853, 484, 289, 811, 202, 1021, 463, 568, 904};
static const int16_t kQzssDelay[10] = {339, 208, 711, 189, 263, 537, 663, 942, 173, 900};

int l1caG2Delay(int prn) {
    if (prn >= 120 && prn <= 158) return kSbasDelay[prn - 120];
    if (prn >= 193 && prn <= 202) return kQzssDelay[prn - 193];
    return -1;
}

int l1caSystem(int prn) {
    if (prn >= 1 && prn <= 32) return GnssGps;
    if (prn >= 120 && prn <= 158) return GnssSbas;
    if (prn >= 193 && prn <= 202) return GnssQzss;
    return -1;
}

bool l1caChips(int prn, uint8_t* chips) {
    if (prn >= 1 && prn <= 32) return gpsCaChips(prn, chips);
    const int d = l1caG2Delay(prn);
    if (d < 0) return false;
    // the same registers as gpsCaChips; the output is G1 xor G2 delayed by d chips (G2i(t) = G2(t - d))
    uint8_t g1s[kGpsCaLen], g2s[kGpsCaLen];
    int g1[11], g2[11];
    for (int i = 1; i <= 10; i++) g1[i] = g2[i] = 1;
    for (int n = 0; n < kGpsCaLen; n++) {
        g1s[n] = (uint8_t)g1[10]; g2s[n] = (uint8_t)g2[10];
        const int f1 = g1[3] ^ g1[10];
        const int f2 = g2[2] ^ g2[3] ^ g2[6] ^ g2[8] ^ g2[9] ^ g2[10];
        for (int i = 10; i > 1; i--) { g1[i] = g1[i - 1]; g2[i] = g2[i - 1]; }
        g1[1] = f1; g2[1] = f2;
    }
    for (int n = 0; n < kGpsCaLen; n++) chips[n] = (uint8_t)(g1s[n] ^ g2s[(n - d + kGpsCaLen) % kGpsCaLen]);
    return true;
}

unsigned l1caFirst10(int prn) {
    uint8_t c[kGpsCaLen];
    if (!l1caChips(prn, c)) return 0;
    unsigned v = 0;
    for (int i = 0; i < 10; i++) v = (v << 1) | c[i];
    return v;
}

static const char* const kE1b[50] = {
#include "gnss_gal_e1b.inc"
};

bool galE1bChips(int prn, uint8_t* chips) {
    if (prn < 1 || prn > 50) return false;
    const char* h = kE1b[prn - 1];
    if (std::strlen(h) != kGalE1Len / 4) return false;
    // ICD Annex C.2: one hexadecimal symbol is four chips, the first chip in time is its most significant bit
    for (int i = 0; i < kGalE1Len / 4; i++) {
        const char c = h[i];
        const int v = c <= '9' ? c - '0' : c - 'A' + 10;
        for (int b = 0; b < 4; b++) chips[4 * i + b] = (uint8_t)((v >> (3 - b)) & 1);
    }
    return true;
}

// G2 output stages per ranging code (0 = no third stage), BDS-SIS-ICD-B1I-3.0 Table 4-1
static const uint8_t kBdsTaps[64][3] = {
    {0, 0, 0}, {1, 3, 0}, {1, 4, 0}, {1, 5, 0}, {1, 6, 0}, {1, 8, 0}, {1, 9, 0}, {1, 10, 0}, {1, 11, 0}, {2, 7, 0}, {3, 4, 0}, {3, 5, 0}, {3, 6, 0}, {3, 8, 0}, {3, 9, 0}, {3, 10, 0},
    {3, 11, 0}, {4, 5, 0}, {4, 6, 0}, {4, 8, 0}, {4, 9, 0}, {4, 10, 0}, {4, 11, 0}, {5, 6, 0}, {5, 8, 0}, {5, 9, 0}, {5, 10, 0}, {5, 11, 0}, {6, 8, 0}, {6, 9, 0}, {6, 10, 0}, {6, 11, 0},
    {8, 9, 0}, {8, 10, 0}, {8, 11, 0}, {9, 10, 0}, {9, 11, 0}, {10, 11, 0}, {1, 2, 7}, {1, 3, 4}, {1, 3, 6}, {1, 3, 8}, {1, 3, 10}, {1, 3, 11}, {1, 4, 5}, {1, 4, 9}, {1, 5, 6}, {1, 5, 8},
    {1, 5, 10}, {1, 5, 11}, {1, 6, 9}, {1, 8, 9}, {1, 9, 10}, {1, 9, 11}, {2, 3, 7}, {2, 5, 7}, {2, 7, 9}, {3, 4, 5}, {3, 4, 9}, {3, 5, 6}, {3, 5, 8}, {3, 5, 10}, {3, 5, 11}, {3, 6, 9}};

bool bdsB1iChips(int prn, uint8_t* chips) {
    if (prn < 1 || prn > kBdsMaxPrn) return false;
    int g1[12], g2[12];
    g2[0] = 0;
    for (int i = 1; i <= 11; i++) g1[i] = g2[i] = (i % 2 == 0) ? 1 : 0;      // 01010101010, stage 1 first
    const int a = kBdsTaps[prn][0], b = kBdsTaps[prn][1], c = kBdsTaps[prn][2];
    for (int n = 0; n < kBdsLen; n++) {
        chips[n] = (uint8_t)(g1[11] ^ g2[a] ^ g2[b] ^ g2[c]);
        const int f1 = g1[1] ^ g1[7] ^ g1[8] ^ g1[9] ^ g1[10] ^ g1[11];
        const int f2 = g2[1] ^ g2[2] ^ g2[3] ^ g2[4] ^ g2[5] ^ g2[8] ^ g2[9] ^ g2[11];
        for (int i = 11; i > 1; i--) { g1[i] = g1[i - 1]; g2[i] = g2[i - 1]; }
        g1[1] = f1; g2[1] = f2;
    }
    return true;
}

void gloCaChips(uint8_t* chips) {
    int s[10];
    for (int i = 1; i <= 9; i++) s[i] = 1;
    for (int n = 0; n < kGloLen; n++) {
        chips[n] = (uint8_t)s[7];
        const int f = s[5] ^ s[9];
        for (int i = 9; i > 1; i--) s[i] = s[i - 1];
        s[1] = f;
    }
}

} // namespace dect2
