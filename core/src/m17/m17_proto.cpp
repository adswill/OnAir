// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// M17 protocol layers (see m17_proto.h), from the M17 specification 2.0.
#include "dect2/m17/m17_proto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "dect2/common/crc.h"
#include "dect2/common/bits.h"
#include "dect2/common/mathconst.h"

namespace dect2 {
namespace m17 {

namespace {

const uint8_t kRand[46] = {0xD6, 0xB5, 0xE2, 0x30, 0x82, 0xFF, 0x84, 0x62, 0xBA, 0x4E, 0x96, 0x90, 0xD8, 0x98, 0xDD, 0x5D,
                           0x0C, 0xC8, 0x52, 0x43, 0x91, 0x1D, 0xF8, 0x6E, 0x68, 0x2F, 0x35, 0xDA, 0x14, 0xEA, 0xCD, 0x76,
                           0x19, 0x8D, 0xD5, 0x80, 0xD1, 0x33, 0x87, 0x13, 0x57, 0x18, 0x2D, 0x29, 0x78, 0xC3};
inline int randBit(int k) { return (kRand[k >> 3] >> (7 - (k & 7))) & 1; }
inline int qpp(int x) { return (45 * x + 92 * x * x) % 368; }

// the rows of P in the generator matrix, for data bit 0 (LSB) .. 11 (MSB)
const uint16_t kGolayP[12] = {0x8EB, 0x93E, 0xA97, 0xDC6, 0x367, 0x6CD, 0xD99, 0x3DA, 0x7B4, 0xF68, 0x63B, 0xC75};
uint16_t golayParity(uint16_t d) {
    uint16_t p = 0;
    for (int i = 0; i < 12; i++) if ((d >> i) & 1) p ^= kGolayP[i];
    return p;
}
struct GolayTable {
    std::vector<uint32_t> pat;   // syndrome -> error pattern (24 bits), 0xFFFFFFFF when none of weight <= 3
    std::vector<uint8_t> w;
    GolayTable() : pat(4096, 0xFFFFFFFFu), w(4096, 99) {
        auto syn = [](uint32_t e) { return (uint16_t)(golayParity((uint16_t)(e >> 12)) ^ (e & 0xFFF)); };
        auto put = [&](uint32_t e, int wt) { const uint16_t s = syn(e); if (wt < w[s]) { w[s] = (uint8_t)wt; pat[s] = e; } };
        put(0, 0);
        for (int a = 0; a < 24; a++) {
            put(1u << a, 1);
            for (int b = a + 1; b < 24; b++) {
                put((1u << a) | (1u << b), 2);
                for (int c = b + 1; c < 24; c++) put((1u << a) | (1u << b) | (1u << c), 3);
            }
        }
    }
};
const GolayTable& golayTable() { static const GolayTable t; return t; }

const uint8_t kP1[61] = {1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1,
                         1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1};
const uint8_t kP2[12] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0};
const uint8_t kP3[8] = {1, 1, 1, 1, 1, 1, 1, 0};

inline int parity5(int v) { return dect2::bits::parity((uint32_t)(v & 0x1F)); }

// conv-encode n bits (flush included by the caller), puncture with p, append to out
void convPunct(const uint8_t* in, int n, const uint8_t* p, int plen, std::vector<uint8_t>& out) {
    int sr = 0, k = 0;
    for (int i = 0; i < n; i++) {
        sr = ((sr << 1) | in[i]) & 0x1F;
        const int g1 = parity5(sr & 0x19), g2 = parity5(sr & 0x17);
        if (p[k++ % plen]) out.push_back((uint8_t)g1);
        if (p[k++ % plen]) out.push_back((uint8_t)g2);
    }
}
// soft depuncture: cnt soft values into 2 * nIn positions
int depunct(const float* in, int cnt, const uint8_t* p, int plen, int nIn, std::vector<float>& out) {
    out.assign((size_t)2 * nIn, 0.f);
    int j = 0;
    for (int k = 0; k < 2 * nIn && j < cnt; k++) if (p[k % plen]) out[(size_t)k] = in[j++];
    return j;
}
void bytesToBits(const uint8_t* b, int nbits, uint8_t* out) { for (int i = 0; i < nbits; i++) out[i] = (b[i >> 3] >> (7 - (i & 7))) & 1; }
void bitsToBytes(const uint8_t* bits, int nbits, uint8_t* b) {
    memset(b, 0, (size_t)(nbits + 7) / 8);
    for (int i = 0; i < nbits; i++) if (bits[i]) b[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
}
void interleave(const std::vector<uint8_t>& t3, uint8_t out[368]) { for (int i = 0; i < 368; i++) out[qpp(i)] = t3[(size_t)i]; }

} // namespace

uint16_t crc16(const uint8_t* d, size_t n) { return (uint16_t)crc::msbBytes<16, 0x5935>(d, n, 0xFFFF); }

uint64_t encodeCallsign(const std::string& cs) {
    if (cs == "@ALL") return kBroadcast;
    uint64_t v = 0;
    const size_t n = std::min<size_t>(cs.size(), 9);
    for (size_t k = n; k-- > 0;) {
        const char c = cs[k];
        unsigned x = 0;
        if (c >= 'A' && c <= 'Z') x = (unsigned)(c - 'A' + 1);
        else if (c >= 'a' && c <= 'z') x = (unsigned)(c - 'a' + 1);
        else if (c >= '0' && c <= '9') x = (unsigned)(c - '0' + 27);
        else if (c == '-') x = 37;
        else if (c == '/') x = 38;
        else if (c == '.') x = 39;
        v = v * 40 + x;
    }
    return v;
}

std::string decodeCallsign(uint64_t v) {
    if (v == kBroadcast) return "@ALL";
    if (v == 0) return "";
    if (v >= 0xEE6B28000000ull) { char b[20]; snprintf(b, sizeof b, "#%012llX", (unsigned long long)v); return b; }
    static const char* cs = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/.";
    std::string s;
    while (v) { s += cs[v % 40]; v /= 40; }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

uint32_t golayEncode(uint16_t d) { d &= 0xFFF; return ((uint32_t)d << 12) | golayParity(d); }

bool golayDecode(uint32_t cw, uint16_t& data, int& errs) {
    cw &= 0xFFFFFF;
    const uint16_t s = (uint16_t)(golayParity((uint16_t)(cw >> 12)) ^ (cw & 0xFFF));
    const GolayTable& t = golayTable();
    if (t.pat[s] == 0xFFFFFFFFu) { data = (uint16_t)(cw >> 12); errs = 4; return false; }
    cw ^= t.pat[s];
    data = (uint16_t)(cw >> 12);
    errs = t.w[s];
    return true;
}

void lsfPack(const Lsf& l, uint8_t o[30]) {
    for (int i = 0; i < 6; i++) { o[i] = (uint8_t)(l.dst >> (40 - 8 * i)); o[6 + i] = (uint8_t)(l.src >> (40 - 8 * i)); }
    o[12] = (uint8_t)(l.type >> 8); o[13] = (uint8_t)l.type;
    memcpy(o + 14, l.meta, 14);
    const uint16_t c = crc16(o, 28);
    o[28] = (uint8_t)(c >> 8); o[29] = (uint8_t)c;
}

bool lsfUnpack(const uint8_t in[30], Lsf& l) {
    l.dst = l.src = 0;
    for (int i = 0; i < 6; i++) { l.dst = (l.dst << 8) | in[i]; l.src = (l.src << 8) | in[6 + i]; }
    l.type = (uint16_t)((in[12] << 8) | in[13]);
    memcpy(l.meta, in + 14, 14);
    return crc16(in, 30) == 0;
}

uint16_t makeType(bool stream, int data, int enc, int sub, int can) {
    return (uint16_t)((stream ? 1 : 0) | (data & 3) << 1 | (enc & 3) << 3 | (sub & 3) << 5 | (can & 15) << 7);
}

std::string typeText(uint16_t t) {
    if (!typeStream(t)) return "packet";
    switch (typeData(t)) {
    case 1: return "data";
    case 2: return "voice 3200";
    case 3: return "voice + data 1600";
    default: return "reserved";
    }
}

std::string encText(uint16_t t) {
    if (!typeStream(t)) return "";
    const int e = typeEnc(t), s = typeEncSub(t);
    if (e == 1) return s == 3 ? "scrambler" : "scrambler " + std::to_string(8 * (s + 1)) + "-bit";
    if (e == 2) return s == 3 ? "AES" : "AES-" + std::to_string(128 + 64 * s);
    if (e == 3) return "other";
    return "";
}

const char* stationText(int st) { return st == 0 ? "fixed" : st == 1 ? "mobile" : st == 2 ? "handheld" : "other"; }

void gnssPack(const Gnss& g, uint8_t m[14]) {
    memset(m, 0, 14);
    m[0] = (uint8_t)((g.source & 15) << 4 | (g.station & 15));
    const int val = (g.posValid ? 8 : 0) | (g.altValid ? 4 : 0) | (g.velValid ? 2 : 0);
    const int br = std::max(0, std::min(359, g.bearing));
    m[1] = (uint8_t)(val << 4 | (g.radius & 7) << 1 | (br >> 8));
    m[2] = (uint8_t)br;
    auto s24 = [](double v) { long x = std::lround(v * 8388607.0); x = std::max(-8388607L, std::min(8388607L, x)); return (uint32_t)x & 0xFFFFFF; };
    const uint32_t la = s24(g.lat / 90), lo = s24(g.lon / 180);
    m[3] = (uint8_t)(la >> 16); m[4] = (uint8_t)(la >> 8); m[5] = (uint8_t)la;
    m[6] = (uint8_t)(lo >> 16); m[7] = (uint8_t)(lo >> 8); m[8] = (uint8_t)lo;
    const long alt = std::max(0L, std::min(65535L, std::lround((g.altM + 500) * 2)));
    m[9] = (uint8_t)(alt >> 8); m[10] = (uint8_t)alt;
    const long sp = std::max(0L, std::min(4095L, std::lround(g.speedKmh * 2)));
    m[11] = (uint8_t)(sp >> 4); m[12] = (uint8_t)((sp & 15) << 4);
}

Gnss gnssUnpack(const uint8_t m[14]) {
    Gnss g;
    g.source = m[0] >> 4; g.station = m[0] & 15;
    g.posValid = m[1] & 0x80; g.altValid = m[1] & 0x40; g.velValid = m[1] & 0x20;
    g.radius = (m[1] >> 1) & 7;
    g.bearing = ((m[1] & 1) << 8) | m[2];
    auto s24 = [](const uint8_t* p) { int32_t x = (p[0] << 16) | (p[1] << 8) | p[2]; if (x & 0x800000) x -= 0x1000000; return x; };
    g.lat = s24(m + 3) * 90.0 / 8388607.0;
    g.lon = s24(m + 6) * 180.0 / 8388607.0;
    g.altM = ((m[9] << 8) | m[10]) * 0.5 - 500;
    g.speedKmh = ((m[11] << 4) | (m[12] >> 4)) * 0.5;
    return g;
}

int textBlockCount(const std::string& msg) { return std::max(1, std::min(4, (int)(msg.size() + 12) / 13)); }

void textBlockPack(const std::string& msg, int block, uint8_t m[14]) {
    const int n = textBlockCount(msg);
    m[0] = (uint8_t)(((1 << n) - 1) << 4 | (1 << (block & 3)));
    for (int i = 0; i < 13; i++) {
        const size_t k = (size_t)block * 13 + (size_t)i;
        m[1 + i] = k < msg.size() ? (uint8_t)msg[k] : (uint8_t)' ';
    }
}

void buildLsf(const uint8_t lsf30[30], uint8_t bits[368]) {
    uint8_t in[244] = {};
    bytesToBits(lsf30, 240, in);
    std::vector<uint8_t> t3;
    convPunct(in, 244, kP1, 61, t3);
    interleave(t3, bits);
}

void buildStream(const uint8_t lsf30[30], int cnt, uint16_t fn, const uint8_t payload[16], uint8_t bits[368]) {
    uint8_t lich[6];
    memcpy(lich, lsf30 + 5 * cnt, 5);
    lich[5] = (uint8_t)((cnt & 7) << 5);
    std::vector<uint8_t> t3;
    for (int k = 0; k < 4; k++) {
        const int b0 = k * 12;   // 12 bits from bit b0 of the LICH
        uint16_t d = 0;
        for (int i = 0; i < 12; i++) d = (uint16_t)(d << 1 | ((lich[(b0 + i) >> 3] >> (7 - ((b0 + i) & 7))) & 1));
        const uint32_t cw = golayEncode(d);
        for (int i = 23; i >= 0; i--) t3.push_back((uint8_t)((cw >> i) & 1));
    }
    uint8_t c[18];
    c[0] = (uint8_t)(fn >> 8); c[1] = (uint8_t)fn;
    memcpy(c + 2, payload, 16);
    uint8_t in[148] = {};
    bytesToBits(c, 144, in);
    convPunct(in, 148, kP2, 12, t3);
    interleave(t3, bits);
}

void buildPacket(const uint8_t chunk[25], bool eof, int counter, uint8_t bits[368]) {
    uint8_t c[26];
    memcpy(c, chunk, 25);
    c[25] = (uint8_t)((eof ? 0x80 : 0) | (counter & 31) << 2);
    uint8_t in[210] = {};
    bytesToBits(c, 206, in);
    std::vector<uint8_t> t3;
    convPunct(in, 210, kP3, 8, t3);
    interleave(t3, bits);
}

std::vector<float> rrcTaps(int sps, int span) {
    const double a = 0.5, pi = dect2::math::kPi;
    const int n = sps * span + 1, c = n / 2;
    std::vector<float> h((size_t)n);
    double e = 0;
    for (int i = 0; i < n; i++) {
        const double t = (double)(i - c) / sps;
        double v;
        if (i == c) v = 1 - a + 4 * a / pi;
        else if (std::fabs(std::fabs(4 * a * t) - 1) < 1e-9) v = a / std::sqrt(2.0) * ((1 + 2 / pi) * std::sin(pi / (4 * a)) + (1 - 2 / pi) * std::cos(pi / (4 * a)));
        else v = (std::sin(pi * t * (1 - a)) + 4 * a * t * std::cos(pi * t * (1 + a))) / (pi * t * (1 - (4 * a * t) * (4 * a * t)));
        h[(size_t)i] = (float)v;
        e += v * v;
    }
    for (auto& v : h) v = (float)(v / std::sqrt(e));   // unit energy: transmit and receive together give 1 at the centre
    return h;
}

void syncSymbols(uint16_t sync, int8_t sym[8]) {
    for (int i = 0; i < 8; i++) {
        const int d = (sync >> (14 - 2 * i)) & 3;
        sym[i] = (int8_t)(d == 1 ? 3 : d == 0 ? 1 : d == 2 ? -1 : -3);
    }
}

void frameSymbols(uint16_t sync, const uint8_t bits[368], int8_t sym[192]) {
    syncSymbols(sync, sym);
    for (int i = 0; i < 184; i++) {
        const int b0 = bits[2 * i] ^ randBit(2 * i), b1 = bits[2 * i + 1] ^ randBit(2 * i + 1);
        const int d = b0 << 1 | b1;
        sym[8 + i] = (int8_t)(d == 1 ? 3 : d == 0 ? 1 : d == 2 ? -1 : -3);
    }
}

void softBits(const float sym[184], float soft[368]) {
    float t4[368];
    for (int i = 0; i < 184; i++) {
        const float s = std::max(-4.f, std::min(4.f, sym[i]));
        t4[2 * i] = -s;                    // MSB: 1 for negative symbols
        t4[2 * i + 1] = std::fabs(s) - 2;  // LSB: 1 for the outer symbols
    }
    for (int k = 0; k < 368; k++) if (randBit(k)) t4[k] = -t4[k];
    for (int i = 0; i < 368; i++) soft[i] = t4[qpp(i)];
}

int viterbi(const float* s2, int nIn, uint8_t* out) {
    constexpr float kNeg = -1e30f;
    std::vector<float> pm(16, kNeg), nm(16);
    pm[0] = 0;
    std::vector<uint8_t> dec((size_t)nIn * 16);   // the input bit that led into each state
    std::vector<uint8_t> prv((size_t)nIn * 16);
    for (int t = 0; t < nIn; t++) {
        std::fill(nm.begin(), nm.end(), kNeg);
        const float a = s2[2 * t], b = s2[2 * t + 1];
        for (int st = 0; st < 16; st++) {
            if (pm[(size_t)st] <= kNeg) continue;
            for (int u = 0; u < 2; u++) {
                const int sr = ((st << 1) | u) & 0x1F;
                const int g1 = parity5(sr & 0x19), g2 = parity5(sr & 0x17);
                const float m = pm[(size_t)st] + (g1 ? a : -a) + (g2 ? b : -b);
                const int ns = sr & 15;
                if (m > nm[(size_t)ns]) { nm[(size_t)ns] = m; dec[(size_t)t * 16 + ns] = (uint8_t)u; prv[(size_t)t * 16 + ns] = (uint8_t)st; }
            }
        }
        pm.swap(nm);
    }
    int st = 0;
    for (int t = nIn - 1; t >= 0; t--) { out[t] = dec[(size_t)t * 16 + st]; st = prv[(size_t)t * 16 + st]; }
    // re-encode and count the received bits that disagree
    int sr = 0, bad = 0;
    for (int t = 0; t < nIn; t++) {
        sr = ((sr << 1) | out[t]) & 0x1F;
        const int g1 = parity5(sr & 0x19), g2 = parity5(sr & 0x17);
        const float a = s2[2 * t], b = s2[2 * t + 1];
        if (a != 0 && (a > 0) != (g1 == 1)) bad++;
        if (b != 0 && (b > 0) != (g2 == 1)) bad++;
    }
    return bad;
}

bool decodeLsf(const float soft[368], uint8_t lsf30[30], int& corrected) {
    std::vector<float> d;
    depunct(soft, 368, kP1, 61, 244, d);
    uint8_t out[244];
    corrected = viterbi(d.data(), 244, out);
    bitsToBytes(out, 240, lsf30);
    return crc16(lsf30, 30) == 0;
}

bool decodeStream(const float soft[368], uint8_t lich6[6], int& golayErrs, uint16_t& fn, uint8_t payload[16], int& corrected) {
    golayErrs = 0;
    bool ok = true;
    uint8_t lb[48];
    for (int k = 0; k < 4; k++) {
        uint32_t cw = 0;
        for (int i = 0; i < 24; i++) cw = cw << 1 | (soft[k * 24 + i] > 0 ? 1u : 0u);
        uint16_t d; int e;
        ok &= golayDecode(cw, d, e);
        golayErrs += e;
        for (int i = 0; i < 12; i++) lb[k * 12 + i] = (uint8_t)((d >> (11 - i)) & 1);
    }
    bitsToBytes(lb, 48, lich6);
    std::vector<float> d;
    depunct(soft + 96, 272, kP2, 12, 148, d);
    uint8_t out[148];
    corrected = viterbi(d.data(), 148, out);
    uint8_t c[18];
    bitsToBytes(out, 144, c);
    fn = (uint16_t)(c[0] << 8 | c[1]);
    memcpy(payload, c + 2, 16);
    return ok;
}

void decodePacket(const float soft[368], uint8_t chunk[25], bool& eof, int& counter, int& corrected) {
    std::vector<float> d;
    depunct(soft, 368, kP3, 8, 210, d);
    uint8_t out[210];
    corrected = viterbi(d.data(), 210, out);
    uint8_t c[26];
    bitsToBytes(out, 206, c);
    memcpy(chunk, c, 25);
    eof = c[25] & 0x80;
    counter = (c[25] >> 2) & 31;
}

void decodeBert(const float soft[368], uint8_t bits197[197], int& corrected) {
    std::vector<float> d;
    depunct(soft, 368, kP2, 12, 201, d);   // 369 punctured bits of which the last is not sent
    uint8_t out[201];
    corrected = viterbi(d.data(), 201, out);
    memcpy(bits197, out, 197);
}

} // namespace m17
} // namespace dect2
