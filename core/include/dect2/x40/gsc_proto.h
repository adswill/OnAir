#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::gsc {

constexpr int kPreambleWords[10] = {2030, 1628, 3198, 647, 191, 3315, 1949, 2540, 1560, 2335};
constexpr int kWord1[50] = {721, 2731, 2952, 1387, 1578, 1708, 2650, 1747, 2580, 1376, 2692, 696, 1667, 3800, 3552, 3424, 1384, 3595, 876, 3124,
                            2285, 2608, 899, 3684, 3129, 2124, 1287, 2616, 1647, 3216, 375, 1232, 2824, 1840, 408, 3127, 3387, 882, 3468, 3267,
                            1575, 3463, 3152, 2572, 1252, 2592, 1552, 835, 1440, 160};
constexpr int kStartCode = 713;
constexpr int kActivationAssumed = 3061;
constexpr int kIllegalLow[16] = {0, 25, 51, 103, 206, 340, 363, 412, 445, 530, 642, 726, 782, 810, 825, 877};
constexpr int kIllegalHigh[7] = {0, 292, 425, 584, 631, 841, 851};
constexpr int kCommaBits = 28, kBlockBits = 121, kPreambleRepeats = 18;
constexpr double kFastBaud = 600, kSlowBaud = 300;

inline uint32_t golayParity(uint32_t d) {
    uint32_t w = (d & 0xFFFu) << 11;
    for (int b = 22; b >= 11; b--) if (w >> b & 1u) w ^= 0xC75u << (b - 11);
    return w & 0x7FFu;
}
inline uint32_t golayWord(uint32_t d) { return ((d & 0xFFFu) << 11) | golayParity(d); }
inline int golayTxBit(uint32_t cw, int k) { return (int)(k < 12 ? (cw >> (11 + k)) & 1u : (cw >> (k - 12)) & 1u); }
inline uint32_t golayFromTx(const int* bits) {
    uint32_t cw = 0;
    for (int k = 0; k < 23; k++) if (bits[k]) cw |= k < 12 ? 1u << (11 + k) : 1u << (k - 12);
    return cw;
}
constexpr uint32_t kAllOnes23 = 0x7FFFFFu;

inline int rev7(int v) { int r = 0; for (int b = 0; b < 7; b++) if (v >> b & 1) r |= 1 << (6 - b); return r; }
inline uint16_t bchWord(int info7) {
    const uint32_t i = (uint32_t)rev7(info7 & 0x7F);
    uint32_t w = i << 8;
    for (int b = 14; b >= 8; b--) if (w >> b & 1u) w ^= 0x1D1u << (b - 8);
    return (uint16_t)((i << 8) | (w & 0xFFu));
}
inline int bchInfo(uint16_t cw) { return rev7((cw >> 8) & 0x7F); }

struct Words { int preamble = 0, w1Index = 0, w2 = 0; bool ok = false; };
inline bool illegalAddress(int g, int a) {
    if (g < 50) { for (int v : kIllegalLow) if (v == a) return true; }
    else for (int v : kIllegalHigh) if (v == a) return true;
    return false;
}
inline Words wordsOfCode(int code6) {
    Words w;
    if (code6 < 0 || code6 > 999999) return w;
    const int i = code6 / 100000, g = code6 / 1000 % 100, a = code6 % 1000, g0 = g % 10;
    w.preamble = (i + g0) % 10;
    w.w1Index = g % 50;
    w.w2 = (a / 50) * 100 + a % 50 + (g >= 50 ? 50 : 0);
    w.ok = !illegalAddress(g, a);
    return w;
}
inline bool codeOfWords(int w1Index, int w2, int& g, int& a) {
    if (w1Index < 0 || w1Index >= 50 || w2 < 0 || w2 > 1999) return false;
    const int hi = w2 / 100, lo = w2 % 100;
    g = w1Index + (lo >= 50 ? 50 : 0);
    a = hi * 50 + lo % 50;
    return a <= 999;
}
inline int functionOf(bool inv1, bool inv2) { return inv1 ? (inv2 ? 4 : 3) : (inv2 ? 2 : 1); }
inline int suffixOf(int function, bool data) { static const int d[4] = {5, 6, 7, 8}, t[4] = {9, 0, 3, 4}; return function >= 1 && function <= 4 ? (data ? d : t)[function - 1] : -1; }

inline int numericValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    switch (c) { case 'U': case 'u': return 11; case ' ': return 12; case '-': return 13; case '=': return 14; case 'E': case 'e': return 15; default: return -1; }
}
inline char numericChar(int v) {
    static const char k[16] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 0, 'U', ' ', '-', '=', 'E'};
    return k[v & 15];
}
constexpr int kNumericNull = 10, kAlphaNull = 0x3E, kAlphaNewline = 0x3C, kAlphaSpare = 0x3F;
inline int alphaValue(char c) {
    unsigned char a = (unsigned char)c;
    if (a == '\n' || a == '\r') return kAlphaNewline;
    if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
    if (a == '\\') return 0x20;
    if (a == '{' || a == '}') return (a & 0x1F) | 0x20;
    if (a >= 0x20 && a <= 0x3F) return a & 0x1F;
    if (a >= 0x61 && a <= 0x7A) return (a & 0x1F) | 0x20;
    return 0;
}
inline int alphaChar(int s) {
    s &= 0x3F;
    if (s == kAlphaNull) return 0;
    if (s == kAlphaNewline) return '\n';
    if (s == kAlphaSpare) return -1;
    if (s < 0x20) return 0x20 | s;
    const int a = 0x60 | (s & 0x1F);
    if (a == 0x60) return '\\';
    if (a >= 'a' && a <= 'z') return a - 'a' + 'A';
    return a;
}

inline void packBlock(const std::vector<int>& sym, int bitsPer, bool more, int info[8]) {
    uint64_t p = 0;
    for (size_t i = 0; i < sym.size() && (int)i * bitsPer < 48; i++) p |= (uint64_t)(sym[i] & ((1 << bitsPer) - 1)) << (i * bitsPer);
    if (more) p |= 1ull << 48;
    int s = 0;
    for (int w = 0; w < 7; w++) { info[w] = (int)(p >> (7 * w) & 0x7F); s += info[w]; }
    info[7] = s & 0x7F;
}
inline bool unpackBlock(const int info[8], int bitsPer, std::vector<int>& sym, bool& more) {
    int s = 0; uint64_t p = 0;
    for (int w = 0; w < 7; w++) { s += info[w]; p |= (uint64_t)(info[w] & 0x7F) << (7 * w); }
    if ((s & 0x7F) != (info[7] & 0x7F)) return false;
    more = (p >> 48 & 1) != 0;
    sym.clear();
    for (int i = 0; i < 48 / bitsPer; i++) sym.push_back((int)(p >> (i * bitsPer) & ((1u << bitsPer) - 1)));
    return true;
}

inline void pushComma(std::vector<int>& out, int n, int first) { for (int i = 0; i < n; i++) out.push_back((first + i) & 1); }
inline void pushSlowWord(std::vector<int>& out, uint32_t cw) { for (int k = 0; k < 23; k++) { const int b = golayTxBit(cw, k); out.push_back(b); out.push_back(b); } }
inline void pushTwoWordBlock(std::vector<int>& out, uint32_t cw1, uint32_t cw2) {
    pushComma(out, kCommaBits, golayTxBit(cw1, 0));
    pushSlowWord(out, cw1);
    out.push_back(1 - golayTxBit(cw2, 0));
    pushSlowWord(out, cw2);
}
inline void pushPreamble(std::vector<int>& out, int index, bool batch) {
    uint32_t cw = golayWord((uint32_t)kPreambleWords[index % 10]);
    if (batch) cw ^= kAllOnes23;
    pushComma(out, kCommaBits, golayTxBit(cw, 0));
    for (int r = 0; r < kPreambleRepeats; r++) pushSlowWord(out, cw);
}
inline void pushStartCode(std::vector<int>& out) { const uint32_t cw = golayWord(kStartCode); pushTwoWordBlock(out, cw, cw ^ kAllOnes23); }
inline void pushActivation(std::vector<int>& out, int word = kActivationAssumed) { const uint32_t cw = golayWord((uint32_t)word); pushTwoWordBlock(out, cw, cw ^ kAllOnes23); }
inline void pushAddress(std::vector<int>& out, int w1Index, int w2, int function) {
    uint32_t c1 = golayWord((uint32_t)kWord1[w1Index % 50]), c2 = golayWord((uint32_t)w2);
    if (function == 3 || function == 4) c1 ^= kAllOnes23;
    if (function == 2 || function == 4) c2 ^= kAllOnes23;
    pushTwoWordBlock(out, c1, c2);
}
inline void pushDataBlock(std::vector<int>& out, const int info[8]) {
    uint16_t cw[8];
    for (int w = 0; w < 8; w++) cw[w] = bchWord(info[w]);
    out.push_back(1 - (int)(cw[0] >> 14 & 1));
    for (int b = 14; b >= 0; b--) for (int w = 0; w < 8; w++) out.push_back((int)(cw[w] >> b & 1));
}
inline int pushMessage(std::vector<int>& out, const std::string& text, bool numeric) {
    const int per = numeric ? 12 : 8, bitsPer = numeric ? 4 : 6;
    const int n = text.empty() ? 1 : (int)((text.size() + per - 1) / per);
    for (int b = 0; b < n; b++) {
        std::vector<int> sym;
        for (int i = 0; i < per; i++) {
            const size_t k = (size_t)(b * per + i);
            if (k < text.size()) { const int v = numeric ? numericValue(text[k]) : alphaValue(text[k]); sym.push_back(v < 0 ? (numeric ? 12 : 0) : v); }
            else sym.push_back(numeric ? kNumericNull : kAlphaNull);
        }
        int info[8];
        packBlock(sym, bitsPer, b + 1 < n, info);
        pushDataBlock(out, info);
    }
    return n;
}

}
