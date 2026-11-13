// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Tone signalling tables: see tones_proto.h.
#include "dect2/tones/tones_proto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {
namespace tones {

namespace {
const double kCtcss[kCtcssN] = {67.0, 69.3, 71.9, 74.4, 77.0, 79.7, 82.5, 85.4, 88.5, 91.5, 94.8, 97.4, 100.0, 103.5, 107.2, 110.9, 114.8, 118.8, 123.0, 127.3, 131.8,
                                136.5, 141.3, 146.2, 151.4, 156.7, 159.8, 162.2, 165.5, 167.9, 171.3, 173.8, 177.3, 179.9, 183.5, 186.2, 189.9, 192.8, 196.6, 199.5, 203.5,
                                206.5, 210.7, 218.1, 225.7, 229.1, 233.6, 241.8, 250.3, 254.1};
const double kExtended[12] = {69.3, 159.8, 165.5, 171.3, 177.3, 183.5, 189.9, 196.6, 199.5, 206.5, 229.1, 254.1};
const int kStdCodes[] = {23, 25, 26, 31, 32, 43, 47, 51, 54, 65, 71, 72, 73, 74, 114, 115, 116, 125, 131, 132, 134, 143, 152, 155, 156, 162, 165, 172, 174, 205, 223, 226, 243, 244,
                         245, 251, 261, 263, 265, 271, 306, 311, 315, 331, 343, 346, 351, 364, 365, 371, 411, 412, 413, 423, 431, 432, 445, 464, 465, 466, 503, 506, 516, 532, 546,
                         565, 606, 612, 624, 627, 631, 632, 654, 662, 664, 703, 712, 723, 731, 732, 734, 743, 754};
constexpr uint32_t kGolay = 0xC75;   // x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1
constexpr uint32_t kMask23 = 0x7FFFFF;

uint32_t golayRem(uint32_t v) {   // v = data(12 bits) << 11 | parity(11 bits): zero for a code word
    for (int i = 22; i >= 11; i--)
        if ((v >> i) & 1) v ^= kGolay << (i - 11);
    return v;
}
int octalValue(int code) {   // 23 (octal digits written in decimal) -> 19
    int v = 0, mul = 1;
    for (int i = 0; i < 3; i++) { v += (code % 10) * mul * 1; mul *= 8; code /= 10; }
    return v & 0x1FF;
}
int octalWritten(int v) { return ((v >> 6) & 7) * 100 + ((v >> 3) & 7) * 10 + (v & 7); }
uint32_t rotl(uint32_t w, int r) { return r == 0 ? w : (((w << r) | (w >> (23 - r))) & kMask23); }
}

double ctcssHz(int idx) { return kCtcss[std::max(0, std::min(kCtcssN - 1, idx))]; }
bool ctcssExtended(int idx) {
    const double f = ctcssHz(idx);
    for (double e : kExtended) if (std::fabs(e - f) < 0.01) return true;
    return false;
}
int ctcssIndex(double hz) {
    int best = -1; double bd = 1.2;
    for (int i = 0; i < kCtcssN; i++) if (std::fabs(kCtcss[i] - hz) < bd) { bd = std::fabs(kCtcss[i] - hz); best = i; }
    return best;
}

uint32_t dcsWord(int code) {
    const uint32_t data = 0x800u | (uint32_t)octalValue(code);
    const uint32_t par = golayRem(data << 11);
    return data | (par << 12);
}
bool dcsValid(uint32_t w, int& code) {
    w &= kMask23;
    const uint32_t data = w & 0xFFF, par = (w >> 12) & 0x7FF;
    if ((data >> 9) != 4u) return false;
    if (golayRem((data << 11) | par) != 0) return false;
    code = octalWritten((int)(data & 0x1FF));
    return true;
}
bool dcsStandard(int code) { return std::binary_search(std::begin(kStdCodes), std::end(kStdCodes), code); }

bool dcsDecode(uint32_t w, DcsHit& out) {
    w &= kMask23;
    struct Cand { int code; bool inv; int rot; };
    std::vector<Cand> c;
    for (int r = 0; r < 23; r++) {
        int code;
        if (dcsValid(rotl(w, r), code)) c.push_back({code, false, r});
        if (dcsValid(rotl(~w & kMask23, r), code)) c.push_back({code, true, r});
    }
    if (c.empty()) return false;
    // best: the normal polarity first, then a code of the standard list, then the first found
    int best = 0, bs = -1;
    for (size_t i = 0; i < c.size(); i++) {
        const int s = (c[i].inv ? 0 : 2) + (dcsStandard(c[i].code) ? 1 : 0);
        if (s > bs) { bs = s; best = (int)i; }
    }
    out = DcsHit();
    out.code = c[(size_t)best].code; out.inverted = c[(size_t)best].inv; out.standard = dcsStandard(out.code);
    for (size_t i = 0; i < c.size(); i++) {
        if ((int)i == best || !dcsStandard(c[i].code)) continue;
        if (out.aliasCode < 0 || (!c[i].inv && out.aliasInverted)) { out.aliasCode = c[i].code; out.aliasInverted = c[i].inv; }
    }
    return true;
}
std::string dcsName(int code, bool inverted) {
    char b[16];
    snprintf(b, sizeof b, "D%03d%c", code, inverted ? 'I' : 'N');
    return b;
}

const SeqSet kSeqSets[] = {
    {"zvei1", "ZVEI1 / PZVEI", {2400, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000, 2200}, 2600, 70},
    {"zvei2", "ZVEI2 / ZVEI3", {2400, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000, 2200}, 970, 70},
    {"ccir", "CCIR", {1981, 1124, 1197, 1275, 1358, 1446, 1540, 1640, 1747, 1860}, 2110, 100},
    {"eea", "EEA", {1981, 1124, 1197, 1275, 1358, 1446, 1540, 1640, 1747, 1860}, 2110, 40},
    {"eia", "EIA", {600, 741, 882, 1023, 1164, 1305, 1446, 1587, 1728, 1869}, 459, 33},
    {"dzvei", "DZVEI", {2200, 970, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000}, 2400, 70},
};
const int kSeqSetN = (int)(sizeof kSeqSets / sizeof kSeqSets[0]);

const std::vector<double>& seqPool() {
    static const std::vector<double> p = [] {
        std::vector<double> v;
        for (const SeqSet& s : kSeqSets) { for (double d : s.digit) v.push_back(d); v.push_back(s.rep); }
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        return v;
    }();
    return p;
}

double selcalHz(int idx) {
    static const double f[kSelcalN] = {312.6, 346.7, 384.6, 426.6, 473.2, 524.8, 582.1, 645.7, 716.1, 794.3, 881.0, 977.2, 1083.9, 1202.3, 1333.5, 1479.1};
    return f[std::max(0, std::min(kSelcalN - 1, idx))];
}
char selcalLetter(int idx) {
    static const char l[kSelcalN + 1] = "ABCDEFGHJKLMPQRS";
    return l[std::max(0, std::min(kSelcalN - 1, idx))];
}

} // namespace tones
} // namespace dect2
