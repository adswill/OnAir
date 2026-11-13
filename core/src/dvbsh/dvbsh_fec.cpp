// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DVB-SH common channel coding (ETSI EN 302 583 V1.2.1 clauses 5.1 - 5.4): CRC-16, EHEADER, padding and EScrambler, the 3GPP2 turbo
// code with the puncturing and tail tables 5.2 / 5.3, the turbo interleaver (5.3.3), the bit-wise interleaver with the 126-of-128 rate
// adaptation (5.4.2) and the time interleaver's branch delays (5.4.3 with TS 102 584 clause 7.2.3.3).
#include "dect2/dvbsh/dvbsh.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

namespace dect2 {
namespace dvbsh {

namespace {
struct RateInfo { const char* name; double rate; int ntcb, nbil, dataCu, ncw, mult; };
const RateInfo kRates[kNumPatterns] = {
    {"1/5", 1.0 / 5, 61440, 60480, 810, 27, 247}, {"2/9", 2.0 / 9, 55296, 54432, 810, 30, 245}, {"1/4", 1.0 / 4, 49152, 48384, 816, 34, 221},
    {"2/7", 2.0 / 7, 43008, 42336, 798, 38, 197}, {"1/3", 1.0 / 3, 36864, 36288, 810, 45, 185}, {"1/3", 1.0 / 3, 36864, 36288, 810, 45, 185},
    {"2/5", 2.0 / 5, 30720, 30240, 810, 54, 167}, {"2/5", 2.0 / 5, 30720, 30240, 810, 54, 167}, {"1/2", 1.0 / 2, 24576, 24192, 816, 68, 157},
    {"1/2", 1.0 / 2, 24576, 24192, 816, 68, 157}, {"2/3", 2.0 / 3, 18432, 18144, 810, 90, 125}, {"2/3", 2.0 / 3, 18432, 18144, 810, 90, 125}};

// Table 5.2: per data bit period the six outputs X Y0 Y1 X' Y'0 Y'1 ('1' passed, '0' deleted), the periods in order
const char* const kDataPat[kNumPatterns] = {
    "111011",
    "101011111011111001111011",
    "111001110011",
    "101001101011101001111001",
    "110010",
    "101001",
    "100000101001001001101001101001001001101001101001001001101001101001001001",
    "110010010010110010110010010010100000110010010010110010110010010010110010",
    "110000100010",
    "100010110000",
    "100000100000100000101001",
    "100000101001100000100000"};
// Table 5.3: the six tail periods (the first three clock encoder 1, the last three encoder 2); a digit is the number of copies
const char* const kTailPat[kNumPatterns] = {
    "311000311000311000000311000311000311",
    "311000311000211000000211000211000311",
    "211000211000211000000211000211000211",
    "111000211000211000000211000111000111",
    "210000210000210000000210000210000210",
    "201000201000201000000201000201000201",
    "111000111000101000000111000111000101",
    "111000110000111000000111000110000111",
    "110000110000110000000110000110000110",
    "101000101000101000000101000101000101",
    "100000101000101000000100000101000101",
    "101000100000100000000101000100000100"};

inline bool validPat(int pat) { return pat >= 0 && pat < kNumPatterns; }
}

const char* rateName(int pat) { return validPat(pat) ? kRates[pat].name : "?"; }
double codeRate(int pat) { return validPat(pat) ? kRates[pat].rate : 0; }
int ntcb(int pat, int K) {
    if (!validPat(pat)) return 0;
    const char* d = kDataPat[pat];
    const int periods = (int)strlen(d) / 6;
    int n = 0;
    for (int t = 0; t < K; t++) for (int o = 0; o < 6; o++) n += d[(t % periods) * 6 + o] - '0';
    for (const char* c = kTailPat[pat]; *c; c++) n += *c - '0';
    return n;
}
int nbil(int pat) { return validPat(pat) ? kRates[pat].nbil : 0; }
int dataCu(int pat) { return validPat(pat) ? kRates[pat].dataCu : 0; }
int codewordsPerShFrame(int pat) { return validPat(pat) ? kRates[pat].ncw : 0; }

// ============================================================================ CRC, PRBS
uint16_t crc16Bits(const uint8_t* bits, size_t n) {
    unsigned reg = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        const unsigned fb = ((reg >> 15) ^ bits[i]) & 1;
        reg = (reg << 1) & 0xFFFF;
        if (fb) reg ^= 0x1021;
    }
    return (uint16_t)reg;
}
uint16_t crc16Bytes(const uint8_t* b, size_t n) {
    unsigned reg = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        reg ^= (unsigned)b[i] << 8;
        for (int k = 0; k < 8; k++) reg = (reg & 0x8000) ? ((reg << 1) ^ 0x1021) & 0xFFFF : (reg << 1) & 0xFFFF;
    }
    return (uint16_t)reg;
}
void prbsBits(size_t n, uint8_t* out) {
    unsigned reg = 0xA9;   // 100101010000000 in stages 1..15 (stage i = bit i-1), as the DVB-T energy dispersal
    for (size_t i = 0; i < n; i++) {
        const unsigned fb = ((reg >> 13) ^ (reg >> 14)) & 1;
        reg = ((reg << 1) | fb) & 0x7FFF;
        out[i] = (uint8_t)fb;
    }
}
namespace {
const std::vector<uint8_t>& eframePrbs() {
    static std::vector<uint8_t> p;
    static std::once_flag once;
    std::call_once(once, [] { p.resize(kTcInput); prbsBits(kTcInput, p.data()); });
    return p;
}
void putBits(uint8_t* b, int& pos, unsigned v, int n) { for (int i = n - 1; i >= 0; i--) b[pos++] = (v >> i) & 1; }
unsigned getBits(const uint8_t* b, int pos, int n) { unsigned v = 0; for (int i = 0; i < n; i++) v = (v << 1) | (b[pos + i] & 1); return v; }
constexpr int kHeaderBits = 114, kUpBits = (187 + 2) * 8;
}

// ============================================================================ EFRAME (5.1, 5.2)
void buildEframe(const uint8_t* pk, int cb, uint8_t* bits) {
    int pos = 0;
    putBits(bits, pos, 3, 2);          // TIS: MPEG-TS
    putBits(bits, pos, 188 * 8, 16);   // UPL
    putBits(bits, pos, 12096, 16);     // DFL
    putBits(bits, pos, 0x47, 8);       // SYNC
    putBits(bits, pos, 0, 32);         // RFU
    putBits(bits, pos, (unsigned)cb & 0xFFFFFF, 24);   // CBCOUNTER (CBCOUNTER_SH 0: no service synchronisation)
    putBits(bits, pos, crc16Bits(bits, 98), 16);
    for (int k = 0; k < kPackets; k++) {
        const uint8_t* p = pk + (size_t)k * 188;
        for (int i = 1; i < 188; i++) putBits(bits, pos, p[i], 8);
        putBits(bits, pos, crc16Bytes(p + 1, 187), 16);
    }
    while (pos < kTcInput) bits[pos++] = 0;   // 72 padding bits
    const auto& r = eframePrbs();
    for (int i = 0; i < kTcInput; i++) bits[i] ^= r[i];
}

EframeInfo parseEframe(const uint8_t* sbits, uint8_t* out) {
    EframeInfo e;
    std::vector<uint8_t> b(kTcInput);
    const auto& r = eframePrbs();
    for (int i = 0; i < kTcInput; i++) b[i] = (sbits[i] ^ r[i]) & 1;
    e.headerOk = crc16Bits(b.data(), 98) == getBits(b.data(), 98, 16) && getBits(b.data(), 0, 2) == 3;
    e.cbcounter = (int)getBits(b.data(), 74, 24);
    const uint8_t sync = e.headerOk ? (uint8_t)getBits(b.data(), 34, 8) : 0x47;
    for (int k = 0; k < kPackets; k++) {
        const int base = kHeaderBits + k * kUpBits;
        uint8_t* p = out + (size_t)k * 188;
        p[0] = sync;
        for (int i = 0; i < 187; i++) p[1 + i] = (uint8_t)getBits(b.data(), base + i * 8, 8);
        e.pktOk[k] = crc16Bytes(p + 1, 187) == getBits(b.data(), base + 187 * 8, 16);
        if (e.pktOk[k]) e.packetsOk++;
        else p[1] |= 0x80;   // transport_error_indicator
    }
    return e;
}

bool eframeChecks(const uint8_t* sbits) {
    const auto& r = eframePrbs();
    uint8_t b[kUpBits];
    for (int i = 0; i < kHeaderBits; i++) b[i] = (sbits[i] ^ r[i]) & 1;
    if (crc16Bits(b, 98) != getBits(b, 98, 16)) return false;
    for (int k = 0; k < kPackets; k++) {
        const int base = kHeaderBits + k * kUpBits;
        for (int i = 0; i < kUpBits; i++) b[i] = (sbits[base + i] ^ r[base + i]) & 1;
        if (crc16Bits(b, 187 * 8) != getBits(b, 187 * 8, 16)) return false;
    }
    return true;
}

// ============================================================================ turbo code (5.3)
const std::vector<int>& turboInterleaver(int K) {
    static std::mutex mu;
    static std::map<int, std::vector<int>> cache;
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(K);
    if (it != cache.end()) return it->second;
    // Table 5.5 for n = 6 (1146 bits) and n = 9 (12282 bits)
    static const int t6[32] = {3, 27, 15, 13, 29, 5, 1, 31, 3, 9, 15, 31, 17, 5, 39, 1, 19, 27, 15, 13, 45, 5, 33, 15, 13, 9, 15, 31, 17, 5, 15, 33};
    static const int t9[32] = {13, 335, 87, 15, 15, 1, 333, 11, 13, 1, 121, 155, 1, 175, 421, 5, 509, 215, 47, 425, 295, 229, 427, 83, 409, 387, 193, 57, 501, 313, 489, 391};
    int n = 0;
    while ((1 << (n + 5)) < K) n++;
    const int* tab = n == 6 ? t6 : t9;   // the two block sizes the standard uses
    std::vector<int> a;
    a.reserve(K);
    const unsigned mask = (1u << n) - 1;
    for (unsigned c = 0; (int)a.size() < K && c < (1u << (n + 5)); c++) {
        const unsigned msb = ((c >> 5) + 1) & mask;
        const unsigned lsb = (msb * (unsigned)tab[c & 31]) & mask;
        unsigned rev = 0;
        for (int i = 0; i < 5; i++) rev |= ((c >> i) & 1u) << (4 - i);
        const unsigned addr = (rev << n) | lsb;
        if ((int)addr < K) a.push_back((int)addr);
    }
    return cache[K] = std::move(a);
}

namespace {
// The RSC: state = a(k-1) | a(k-2) << 1 | a(k-3) << 2; d = 1 + D2 + D3, n0 = 1 + D + D3, n1 = 1 + D + D2 + D3
struct Trellis {
    int next[8][2], p0[8][2], p1[8][2], tailU[8];
    Trellis() {
        for (int s = 0; s < 8; s++) {
            const int a1 = s & 1, a2 = (s >> 1) & 1, a3 = (s >> 2) & 1;
            for (int u = 0; u < 2; u++) {
                const int a = u ^ a2 ^ a3;
                next[s][u] = a | (a1 << 1) | (a2 << 2);
                p0[s][u] = a ^ a1 ^ a3;
                p1[s][u] = a ^ a1 ^ a2 ^ a3;
            }
            tailU[s] = a2 ^ a3;   // the switch down: the register input becomes zero
        }
    }
};
const Trellis& trellis() { static const Trellis t; return t; }

// Where each transmitted symbol comes from: stream (0 X, 1 Y0, 2 Y1, 3 X', 4 Y'0, 5 Y'1) and period (0..K+2, the tail periods of encoder 1
// and of encoder 2 each counted K, K+1, K+2)
const std::vector<std::pair<uint8_t, int>>& outputMap(int K, int pat) {
    static std::mutex mu;
    static std::map<std::pair<int, int>, std::vector<std::pair<uint8_t, int>>> cache;
    std::lock_guard<std::mutex> lk(mu);
    auto key = std::make_pair(K, pat);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    std::vector<std::pair<uint8_t, int>> m;
    const char* d = kDataPat[pat];
    const int periods = (int)strlen(d) / 6;
    for (int t = 0; t < K; t++)
        for (int o = 0; o < 6; o++) if (d[(t % periods) * 6 + o] == '1') m.push_back({(uint8_t)o, t});
    const char* tl = kTailPat[pat];
    for (int t = 0; t < 6; t++)
        for (int o = 0; o < 6; o++)
            for (int c = 0; c < tl[t * 6 + o] - '0'; c++) m.push_back({(uint8_t)o, K + t % 3});
    return cache[key] = std::move(m);
}

inline float maxs(float a, float b) {   // max* with a linear correction of ln(1 + e^-|a-b|)
    const float d = std::fabs(a - b);
    return std::max(a, b) + std::max(0.f, 0.6931f - 0.3f * d);
}

// One constituent decoder. ls: systematic channel LLR (K + 3, the tail included), la: a-priori (K), lp0/lp1: parities (K + 3).
// le: extrinsic out (K), app: a-posteriori out (K).
void sisoGeneral(const float* ls, const float* la, const float* lp0, const float* lp1, int K, std::vector<float>& alpha, float* le, float* app) {
    const Trellis& T = trellis();
    const int N = K + 3;
    const float NEG = -1e30f;
    alpha.assign((size_t)(N + 1) * 8, NEG);
    alpha[0] = 0;
    auto gamma = [&](int k, int s, int u) {
        const float lu = ls[k] + (k < K ? la[k] : 0.f);
        return 0.5f * ((u ? -lu : lu) + (T.p0[s][u] ? -lp0[k] : lp0[k]) + (T.p1[s][u] ? -lp1[k] : lp1[k]));
    };
    for (int k = 0; k < N; k++) {
        float* an = &alpha[(size_t)(k + 1) * 8];
        const float* ac = &alpha[(size_t)k * 8];
        for (int s = 0; s < 8; s++) {
            if (ac[s] <= NEG * 0.5f) continue;
            for (int u = 0; u < 2; u++) {
                if (k >= K && u != T.tailU[s]) continue;
                const int ns = T.next[s][u];
                const float v = ac[s] + gamma(k, s, u);
                an[ns] = an[ns] <= NEG * 0.5f ? v : maxs(an[ns], v);
            }
        }
        const float ref = an[0] > NEG * 0.5f ? an[0] : *std::max_element(an, an + 8);
        for (int s = 0; s < 8; s++) if (an[s] > NEG * 0.5f) an[s] -= ref;
    }
    float beta[8], bn[8];
    for (int s = 0; s < 8; s++) beta[s] = s == 0 ? 0.f : NEG;
    for (int k = N - 1; k >= 0; k--) {
        const float* ac = &alpha[(size_t)k * 8];
        float m0 = NEG, m1 = NEG;
        for (int s = 0; s < 8; s++) bn[s] = NEG;
        for (int s = 0; s < 8; s++) {
            for (int u = 0; u < 2; u++) {
                if (k >= K && u != T.tailU[s]) continue;
                const int ns = T.next[s][u];
                if (beta[ns] <= NEG * 0.5f) continue;
                const float g = gamma(k, s, u);
                const float b = beta[ns] + g;
                bn[s] = bn[s] <= NEG * 0.5f ? b : maxs(bn[s], b);
                if (k < K && ac[s] > NEG * 0.5f) {
                    const float m = ac[s] + b;
                    if (u) m1 = m1 <= NEG * 0.5f ? m : maxs(m1, m);
                    else m0 = m0 <= NEG * 0.5f ? m : maxs(m0, m);
                }
            }
        }
        const float ref = bn[0] > NEG * 0.5f ? bn[0] : *std::max_element(bn, bn + 8);
        for (int s = 0; s < 8; s++) beta[s] = bn[s] > NEG * 0.5f ? bn[s] - ref : NEG;
        if (k < K) {
            const float L = std::max(-80.f, std::min(80.f, m0 - m1));
            app[k] = L;
            le[k] = L - ls[k] - la[k];
        }
    }
}
}

// The same recursions as sisoGeneral, with the branch metrics of each period worked out once (the same expression) and, where every state
// is reachable (all but the first three and the tail periods), each new state metric taken from its two predecessors directly. max* is
// symmetric (max and |a - b|), so the result is the same bit for bit; the per-state checks were most of the time (4.1, docs/CPU-4.1.md).
void siso(const float* ls, const float* la, const float* lp0, const float* lp1, int K, std::vector<float>& alpha, float* le, float* app) {
    const Trellis& T = trellis();
    const int N = K + 3;
    if (K < 8) { sisoGeneral(ls, la, lp0, lp1, K, alpha, le, app); return; }
    struct Pred { int s[2], u[2]; };
    static const auto pred = [] {
        const Trellis& t = trellis();
        std::array<Pred, 8> p{};
        int cnt[8] = {};
        for (int s = 0; s < 8; s++) for (int u = 0; u < 2; u++) { const int ns = t.next[s][u]; p[(size_t)ns].s[cnt[ns]] = s; p[(size_t)ns].u[cnt[ns]] = u; cnt[ns]++; }
        return p;
    }();
    const float NEG = -1e30f;
    alpha.assign((size_t)(N + 1) * 8, NEG);
    alpha[0] = 0;
    auto gam = [&](int k, float* G) {   // G[u * 4 + p0 * 2 + p1]
        const float lu = ls[k] + (k < K ? la[k] : 0.f);
        for (int u = 0; u < 2; u++) for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) G[u * 4 + a * 2 + b] = 0.5f * ((u ? -lu : lu) + (a ? -lp0[k] : lp0[k]) + (b ? -lp1[k] : lp1[k]));
    };
    float G[8];
    for (int k = 0; k < N; k++) {
        float* an = &alpha[(size_t)(k + 1) * 8];
        const float* ac = &alpha[(size_t)k * 8];
        gam(k, G);
        if (k >= 3 && k < K) {
            for (int ns = 0; ns < 8; ns++) {
                const Pred& q = pred[(size_t)ns];
                const int s0 = q.s[0], u0 = q.u[0], s1 = q.s[1], u1 = q.u[1];
                const float v0 = ac[s0] + G[u0 * 4 + T.p0[s0][u0] * 2 + T.p1[s0][u0]];
                const float v1 = ac[s1] + G[u1 * 4 + T.p0[s1][u1] * 2 + T.p1[s1][u1]];
                an[ns] = maxs(v0, v1);
            }
            const float ref = an[0];
            for (int st = 0; st < 8; st++) an[st] -= ref;
            continue;
        }
        for (int st = 0; st < 8; st++) {
            if (ac[st] <= NEG * 0.5f) continue;
            for (int u = 0; u < 2; u++) {
                if (k >= K && u != T.tailU[st]) continue;
                const int ns = T.next[st][u];
                const float v = ac[st] + G[u * 4 + T.p0[st][u] * 2 + T.p1[st][u]];
                an[ns] = an[ns] <= NEG * 0.5f ? v : maxs(an[ns], v);
            }
        }
        const float ref = an[0] > NEG * 0.5f ? an[0] : *std::max_element(an, an + 8);
        for (int st = 0; st < 8; st++) if (an[st] > NEG * 0.5f) an[st] -= ref;
    }
    float beta[8], bn[8];
    for (int st = 0; st < 8; st++) beta[st] = st == 0 ? 0.f : NEG;
    for (int k = N - 1; k >= 0; k--) {
        const float* ac = &alpha[(size_t)k * 8];
        gam(k, G);
        float m0 = NEG, m1 = NEG;
        bool full = k >= 3 && k < K;
        for (int st = 0; st < 8 && full; st++) full = beta[st] > NEG * 0.5f;
        if (full) {
            float b0[8], b1[8];
            for (int st = 0; st < 8; st++) {
                b0[st] = beta[T.next[st][0]] + G[0 * 4 + T.p0[st][0] * 2 + T.p1[st][0]];
                b1[st] = beta[T.next[st][1]] + G[1 * 4 + T.p0[st][1] * 2 + T.p1[st][1]];
                bn[st] = maxs(b0[st], b1[st]);
            }
            m0 = ac[0] + b0[0]; m1 = ac[0] + b1[0];
            for (int st = 1; st < 8; st++) { m0 = maxs(m0, ac[st] + b0[st]); m1 = maxs(m1, ac[st] + b1[st]); }
        } else {
            for (int st = 0; st < 8; st++) bn[st] = NEG;
            for (int st = 0; st < 8; st++) {
                for (int u = 0; u < 2; u++) {
                    if (k >= K && u != T.tailU[st]) continue;
                    const int ns = T.next[st][u];
                    if (beta[ns] <= NEG * 0.5f) continue;
                    const float b = beta[ns] + G[u * 4 + T.p0[st][u] * 2 + T.p1[st][u]];
                    bn[st] = bn[st] <= NEG * 0.5f ? b : maxs(bn[st], b);
                    if (k < K && ac[st] > NEG * 0.5f) {
                        const float m = ac[st] + b;
                        if (u) m1 = m1 <= NEG * 0.5f ? m : maxs(m1, m);
                        else m0 = m0 <= NEG * 0.5f ? m : maxs(m0, m);
                    }
                }
            }
        }
        const float ref = bn[0] > NEG * 0.5f ? bn[0] : *std::max_element(bn, bn + 8);
        for (int st = 0; st < 8; st++) beta[st] = bn[st] > NEG * 0.5f ? bn[st] - ref : NEG;
        if (k < K) {
            const float L = std::max(-80.f, std::min(80.f, m0 - m1));
            app[k] = L;
            le[k] = L - ls[k] - la[k];
        }
    }
}
void turboEncode(const uint8_t* info, int K, int pat, std::vector<uint8_t>& coded) {
    const Trellis& T = trellis();
    const auto& addr = turboInterleaver(K);
    const int N = K + 3;
    std::vector<uint8_t> out[6];
    for (auto& o : out) o.assign(N, 0);
    int s1 = 0, s2 = 0;
    for (int k = 0; k < K; k++) {
        const int u = info[k] & 1, v = info[addr[k]] & 1;
        out[0][k] = (uint8_t)u; out[1][k] = (uint8_t)T.p0[s1][u]; out[2][k] = (uint8_t)T.p1[s1][u]; s1 = T.next[s1][u];
        out[3][k] = (uint8_t)v; out[4][k] = (uint8_t)T.p0[s2][v]; out[5][k] = (uint8_t)T.p1[s2][v]; s2 = T.next[s2][v];
    }
    for (int k = K; k < N; k++) {
        int u = T.tailU[s1];
        out[0][k] = (uint8_t)u; out[1][k] = (uint8_t)T.p0[s1][u]; out[2][k] = (uint8_t)T.p1[s1][u]; s1 = T.next[s1][u];
        u = T.tailU[s2];
        out[3][k] = (uint8_t)u; out[4][k] = (uint8_t)T.p0[s2][u]; out[5][k] = (uint8_t)T.p1[s2][u]; s2 = T.next[s2][u];
    }
    const auto& m = outputMap(K, pat);
    coded.resize(m.size());
    for (size_t i = 0; i < m.size(); i++) coded[i] = out[m[i].first][m[i].second];
}

int turboDecode(const float* llr, int K, int pat, int maxIter, std::vector<uint8_t>& info, const std::function<bool(const uint8_t*)>& stop, std::vector<float>* appOut) {
    const auto& addr = turboInterleaver(K);
    const auto& m = outputMap(K, pat);
    const int N = K + 3;
    std::vector<float> L[6];
    for (auto& v : L) v.assign(N, 0.f);
    for (size_t i = 0; i < m.size(); i++) L[m[i].first][m[i].second] += llr[i];   // repeated tail symbols add up
    std::vector<float> ls2(N), la1(K, 0.f), la2(K), le1(K), le2(K), app1(K), app2(K), alpha;
    for (int k = 0; k < K; k++) ls2[k] = L[0][addr[k]] + L[3][k];
    for (int k = K; k < N; k++) ls2[k] = L[3][k];
    info.assign(K, 0);
    int it = 0;
    for (it = 1; it <= std::max(1, maxIter); it++) {
        siso(L[0].data(), la1.data(), L[1].data(), L[2].data(), K, alpha, le1.data(), app1.data());
        for (int k = 0; k < K; k++) la2[k] = le1[addr[k]];
        siso(ls2.data(), la2.data(), L[4].data(), L[5].data(), K, alpha, le2.data(), app2.data());
        for (int k = 0; k < K; k++) { la1[addr[k]] = le2[k]; app1[addr[k]] = app2[k]; }
        for (int k = 0; k < K; k++) info[k] = app1[k] < 0 ? 1 : 0;
        if (stop && stop(info.data())) break;
    }
    if (appOut) *appOut = app1;
    return std::min(it, std::max(1, maxIter));
}

// ============================================================================ bit-wise interleaver (5.4.2)
void bitInterleave(const uint8_t* coded, int pat, uint8_t* out) {
    const int n = kRates[pat].ntcb, nb = kRates[pat].nbil, mult = kRates[pat].mult;
    for (int i = 0; i < nb; i++) {
        const long w = (long)(i / 126) * 128 + i % 126;
        out[i] = coded[(mult * w) % n];
    }
}
void bitDeinterleave(const float* llr, int pat, float* coded) {
    const int n = kRates[pat].ntcb, nb = kRates[pat].nbil, mult = kRates[pat].mult;
    std::fill(coded, coded + n, 0.f);
    for (int i = 0; i < nb; i++) {
        const long w = (long)(i / 126) * 128 + i % 126;
        coded[(mult * w) % n] = llr[i];
    }
}
void encodeCodeword(const uint8_t* eframe, int pat, std::vector<uint8_t>& out) {
    std::vector<uint8_t> coded;
    turboEncode(eframe, kTcInput, pat, coded);
    out.resize(nbil(pat));
    bitInterleave(coded.data(), pat, out.data());
}

// ============================================================================ time interleaver delays
bool tiDelays(const TiConfig& c, int dataCuPerSh, std::vector<int>& L) {
    L.assign(kBranches, 0);
    if (c.commonMultiplier < 1 || c.commonMultiplier > 63 || c.lateTaps < 0 || c.lateTaps > 48 || c.slices < 1 || c.slices > 63 || c.sliceDistance < 0 ||
        c.sliceDistance > 255 || c.nonLateIncrement < 0 || c.nonLateIncrement > 63)
        return false;
    for (int j = 0; j < c.lateTaps; j++) L[j] = j * c.commonMultiplier;
    if (c.lateTaps == 48) return true;
    const int nonLateSlices = c.lateTaps == 0 ? c.slices : c.slices - 1;
    if (nonLateSlices <= 0 || (48 - c.lateTaps) % nonLateSlices) return false;
    const int perSlice = (48 - c.lateTaps) / nonLateSlices, lateSlices = c.slices - nonLateSlices;
    const int sliceOffset = dataCuPerSh / 3 * c.sliceDistance;   // the SH frame capacity in IU / 48, times the distance
    for (int s = 0; s < nonLateSlices; s++)
        for (int i = 0; i < perSlice; i++) L[c.lateTaps + s * perSlice + i] = (s + lateSlices) * sliceOffset + i * c.commonMultiplier * c.nonLateIncrement;
    return true;
}

} // namespace dvbsh
} // namespace dect2
