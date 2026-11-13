// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// dPMR air interface helpers (see dpmr_proto.h).
#include "dect2/dpmr/dpmr_proto.h"
#include "dect2/common/bits.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dect2 {
namespace dpmr {

namespace {
template <int N> struct Sync {
    int s[N];
    constexpr Sync(uint64_t v) : s{} { for (int i = 0; i < N; i++) { const int d = (int)((v >> (2 * (N - 1 - i))) & 3); s[i] = d == 1 ? 3 : d == 0 ? 1 : d == 2 ? -1 : -3; } }
};
constexpr Sync<kFsLong> gFs1(0x57FF5F75D577ull), gFs3(0x7DFFD5F55D5Full), gFs4(0xFD55F5DF7FDDull);
constexpr Sync<kFsShort> gFs2(0x5FF7D7ull);
const uint8_t kSyn[8] = {0x3, 0x5, 0x6, 0x7, 0x9, 0xA, 0xB, 0xC};   // the parity pattern (syndrome) of each data bit
uint32_t crcGen(const uint8_t* b, int n, int deg, uint32_t poly, uint32_t init) {
    uint32_t r = init;
    const uint32_t top = 1u << (deg - 1), mask = (1u << deg) - 1;
    for (int i = 0; i < n; i++) { const bool f = ((r & top) != 0) != (b[i] != 0); r = (r << 1) & mask; if (f) r ^= poly; }
    return r;
}
void put(uint8_t* b, int& p, uint32_t v, int n) { for (int i = n - 1; i >= 0; i--) b[p++] = (uint8_t)((v >> i) & 1); }
uint32_t get(const uint8_t* b, int& p, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v = (v << 1) | b[p++]; return v; }
} // namespace

const int kFs1[kFsLong] = {gFs1.s[0], gFs1.s[1], gFs1.s[2], gFs1.s[3], gFs1.s[4], gFs1.s[5], gFs1.s[6], gFs1.s[7], gFs1.s[8], gFs1.s[9], gFs1.s[10], gFs1.s[11],
                           gFs1.s[12], gFs1.s[13], gFs1.s[14], gFs1.s[15], gFs1.s[16], gFs1.s[17], gFs1.s[18], gFs1.s[19], gFs1.s[20], gFs1.s[21], gFs1.s[22], gFs1.s[23]};
const int kFs3[kFsLong] = {gFs3.s[0], gFs3.s[1], gFs3.s[2], gFs3.s[3], gFs3.s[4], gFs3.s[5], gFs3.s[6], gFs3.s[7], gFs3.s[8], gFs3.s[9], gFs3.s[10], gFs3.s[11],
                           gFs3.s[12], gFs3.s[13], gFs3.s[14], gFs3.s[15], gFs3.s[16], gFs3.s[17], gFs3.s[18], gFs3.s[19], gFs3.s[20], gFs3.s[21], gFs3.s[22], gFs3.s[23]};
const int kFs4[kFsLong] = {gFs4.s[0], gFs4.s[1], gFs4.s[2], gFs4.s[3], gFs4.s[4], gFs4.s[5], gFs4.s[6], gFs4.s[7], gFs4.s[8], gFs4.s[9], gFs4.s[10], gFs4.s[11],
                           gFs4.s[12], gFs4.s[13], gFs4.s[14], gFs4.s[15], gFs4.s[16], gFs4.s[17], gFs4.s[18], gFs4.s[19], gFs4.s[20], gFs4.s[21], gFs4.s[22], gFs4.s[23]};
const int kFs2[kFsShort] = {gFs2.s[0], gFs2.s[1], gFs2.s[2], gFs2.s[3], gFs2.s[4], gFs2.s[5], gFs2.s[6], gFs2.s[7], gFs2.s[8], gFs2.s[9], gFs2.s[10], gFs2.s[11]};

void ccSymbols(int cc, int out[kCcSym]) {
    for (int i = 0; i < 6; i++) { const bool b = (cc >> (5 - i)) & 1; out[2 * i] = b ? 3 : -3; out[2 * i + 1] = b ? -3 : 3; }
}
int ccDecode(const float* v, float* score) {
    int cc = 0; float sc = 0;
    for (int i = 0; i < 6; i++) { const float d = v[2 * i] - v[2 * i + 1]; if (d > 0) cc |= 1 << (5 - i); sc += std::fabs(d); }
    if (score) *score = sc / 36.f;   // 1 for a clean code
    return cc;
}

uint32_t crc8(const uint8_t* b, int n) { return crcGen(b, n, 8, 0x07, 0); }
uint32_t crc7(const uint8_t* b, int n) { return crcGen(b, n, 7, 0x09, 0); }
uint32_t crc16(const uint8_t* b, int n) { return crcGen(b, n, 16, 0x1021, 0xFFFF); }

void hamEncode(const uint8_t d[8], uint8_t c[12]) {
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) { c[i] = d[i] & 1; if (c[i]) p ^= kSyn[i]; }
    for (int i = 0; i < 4; i++) c[8 + i] = (p >> (3 - i)) & 1;
}
int hamDecode(uint8_t c[12]) {
    uint8_t s = 0;
    for (int i = 0; i < 8; i++) if (c[i]) s ^= kSyn[i];
    for (int i = 0; i < 4; i++) if (c[8 + i]) s ^= (uint8_t)(1 << (3 - i));
    if (!s) return 0;
    for (int i = 0; i < 8; i++) if (kSyn[i] == s) { c[i] ^= 1; return 1; }
    for (int i = 0; i < 4; i++) if ((1 << (3 - i)) == s) { c[8 + i] ^= 1; return 1; }
    return -1;
}

void pn9(uint8_t* out, int n) {
    uint32_t s = 0x1FF;
    for (int i = 0; i < n; i++) { out[i] = (uint8_t)(s & 1); const uint32_t f = (s ^ (s >> 5)) & 1; s = (s >> 1) | (f << 8); }
}

void blockEncode(const uint8_t* info, int nW, uint8_t* air) {
    uint8_t c[12], pn[512];
    pn9(pn, 12 * nW);
    for (int w = 0; w < nW; w++) { hamEncode(info + 8 * w, c); for (int k = 0; k < 12; k++) air[k * nW + w] = c[k]; }
    for (int i = 0; i < 12 * nW; i++) air[i] ^= pn[i];
}
// Soft decision: every word is the codeword closest to the received values (all 256 tried, the correlation with the soft values), which
// also repairs most double errors that the hard decoder can only detect. A word more than two bits from the hard decisions is not trusted.
int blockDecodeSoft(const float* soft, int nW, uint8_t* info) {
    static const struct Table {
        uint16_t cw[256];
        Table() {
            for (int v = 0; v < 256; v++) {
                uint8_t d[8], c[12];
                for (int i = 0; i < 8; i++) d[i] = (uint8_t)((v >> (7 - i)) & 1);
                hamEncode(d, c);
                uint16_t w = 0;
                for (int k = 0; k < 12; k++) w = (uint16_t)((w << 1) | c[k]);
                cw[v] = w;
            }
        }
    } tab;
    uint8_t pn[512];
    pn9(pn, 12 * nW);
    int fixed = 0;
    bool bad = false;
    for (int w = 0; w < nW; w++) {
        float s[12];
        uint16_t hard = 0;
        for (int k = 0; k < 12; k++) {
            s[k] = pn[k * nW + w] ? -soft[k * nW + w] : soft[k * nW + w];
            hard = (uint16_t)((hard << 1) | (s[k] > 0 ? 1 : 0));
        }
        int best = 0;
        float bm = -1e30f;
        for (int v = 0; v < 256; v++) {
            const uint16_t c = tab.cw[v];
            float m = 0;
            for (int k = 0; k < 12; k++) m += ((c >> (11 - k)) & 1) ? s[k] : -s[k];
            if (m > bm) { bm = m; best = v; }
        }
        const int dist = bits::popcount((uint32_t)(tab.cw[best] ^ hard));
        if (dist > 2) bad = true; else fixed += dist;
        for (int i = 0; i < 8; i++) info[8 * w + i] = (uint8_t)((best >> (7 - i)) & 1);
    }
    return bad ? -1 : fixed;
}

int blockDecode(const uint8_t* air, int nW, uint8_t* info) {
    uint8_t c[12], pn[512];
    pn9(pn, 12 * nW);
    int fixed = 0; bool bad = false;
    for (int w = 0; w < nW; w++) {
        for (int k = 0; k < 12; k++) c[k] = (air[k * nW + w] ^ pn[k * nW + w]) & 1;
        const int r = hamDecode(c);
        if (r < 0) bad = true; else fixed += r;
        memcpy(info + 8 * w, c, 8);
    }
    return bad ? -1 : fixed;
}

void headerBits(const Header& h, uint8_t b[80]) {
    int p = 0;
    put(b, p, (uint32_t)h.type, 4); put(b, p, h.called & 0xFFFFFF, 24); put(b, p, h.own & 0xFFFFFF, 24);
    put(b, p, (uint32_t)h.commMode, 3); put(b, p, (uint32_t)h.callType, 2); put(b, p, h.emergency, 1); put(b, p, (uint32_t)h.extra & 0xFF, 8); put(b, p, 0, 6);
    put(b, p, crc8(b, 72), 8);
}
bool headerParse(const uint8_t b[80], Header& h) {
    int p = 72;
    if (get(b, p, 8) != crc8(b, 72)) return false;
    p = 0;
    h.type = (int)get(b, p, 4); h.called = get(b, p, 24); h.own = get(b, p, 24);
    h.commMode = (int)get(b, p, 3); h.callType = (int)get(b, p, 2); h.emergency = get(b, p, 1); h.extra = (int)get(b, p, 8);
    return h.type <= kHdrStatus && h.commMode <= kVoiceData && h.callType <= kAllCall;
}

void cchBits(const Cch& c, uint8_t b[48]) {
    int p = 0;
    put(b, p, (uint32_t)c.frame, 2); put(b, p, (uint32_t)c.idPart, 12); put(b, p, (uint32_t)c.commMode, 3); put(b, p, (uint32_t)c.version, 2);
    put(b, p, (uint32_t)c.callType, 2); put(b, p, c.emergency, 1); put(b, p, 0, 1); put(b, p, c.slowData & 0x3FFFF, 18);
    put(b, p, crc7(b, 41), 7);
}
bool cchParse(const uint8_t b[48], Cch& c) {
    int p = 41;
    if (get(b, p, 7) != crc7(b, 41)) return false;
    p = 0;
    c.frame = (int)get(b, p, 2); c.idPart = (int)get(b, p, 12); c.commMode = (int)get(b, p, 3); c.version = (int)get(b, p, 2);
    c.callType = (int)get(b, p, 2); c.emergency = get(b, p, 1); get(b, p, 1); c.slowData = get(b, p, 18);
    return c.commMode <= kVoiceData;
}

void endBits(const End& e, uint8_t b[48]) {
    int p = 0;
    put(b, p, (uint32_t)e.type, 2); put(b, p, e.own & 0xFFFFFF, 24); put(b, p, 0, 15); put(b, p, crc7(b, 41), 7);
}
bool endParse(const uint8_t b[48], End& e) {
    int p = 41;
    if (get(b, p, 7) != crc7(b, 41)) return false;
    p = 0;
    e.type = (int)get(b, p, 2); e.own = get(b, p, 24);
    return true;
}

void dataBits(int block, const std::string& t, uint8_t b[192]) {
    int p = 0;
    put(b, p, (uint32_t)block & 0xFF, 8);
    for (int i = 0; i < 21; i++) put(b, p, i < (int)t.size() ? (uint8_t)t[(size_t)i] : 0, 8);
    put(b, p, crc16(b, 176), 16);
}
bool dataParse(const uint8_t b[192], int& block, std::string& t) {
    int p = 176;
    if (get(b, p, 16) != crc16(b, 176)) return false;
    p = 0;
    block = (int)get(b, p, 8);
    t.clear();
    for (int i = 0; i < 21; i++) { const char ch = (char)get(b, p, 8); if (ch) t += (ch >= 32 && ch < 127) ? ch : '.'; }
    return true;
}

const char* hdrTypeName(int t) {
    static const char* n[] = {"call", "connection request", "unconnect", "acknowledge", "system request", "status"};
    return t >= 0 && t <= kHdrStatus ? n[t] : "?";
}
const char* commModeName(int m) {
    static const char* n[] = {"voice", "voice + slow data", "data type 1", "data type 2", "packet data", "voice + data"};
    return m >= 0 && m <= kVoiceData ? n[m] : "?";
}
const char* callTypeName(int t) { return t == kIndividual ? "individual" : t == kGroup ? "group" : t == kAllCall ? "all call" : "?"; }
double pmr446Mhz(int ch) { return 446.103125 + (ch - 1) * 0.00625; }
std::string idText(uint32_t id) {
    if (id == 0xFFFFFF) return "ALL";
    char b[16];
    snprintf(b, sizeof b, "%08u", id);
    return b;
}

} // namespace dpmr
} // namespace dect2
