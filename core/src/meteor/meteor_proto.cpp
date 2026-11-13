// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meteor-M LRPT layers above the demodulator (see meteor_proto.h).
#include "dect2/meteor/meteor_proto.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include "dect2/common/bits.h"

namespace dect2::meteor {

namespace {

inline int parity8(unsigned x) { return dect2::bits::parity(x & 0xFFu); }

constexpr unsigned kPolyA = 0x4F, kPolyB = 0x6D;

// --- GF(256), x^8 + x^7 + x^2 + x + 1 ----------------------------------------------------------------------------------------------
struct Gf {
    uint8_t exp[512];
    int log[256];
    uint8_t tal[256], tal1[256];     // conventional -> dual basis, dual -> conventional
    uint8_t gen[33];                 // generator, highest power first (gen[0] = 1)
    Gf() {
        int x = 1;
        for (int i = 0; i < 255; i++) { exp[i] = (uint8_t)x; log[x] = i; x <<= 1; if (x & 0x100) x ^= 0x187; }
        for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
        log[0] = -1;
        static const uint8_t t[8] = {0x8d, 0xef, 0xec, 0x86, 0xfa, 0x99, 0xaf, 0x7b};
        for (int i = 0; i < 256; i++) {
            uint8_t v = 0;
            for (int k = 0; k < 8; k++) if (i & (1 << k)) v ^= t[7 - k];
            tal[i] = v; tal1[v] = (uint8_t)i;
        }
        // g(x) = prod (x - a^(11 (112 + i))), i = 0 .. 31
        uint8_t g[33] = {1};
        int deg = 0;
        for (int i = 0; i < 32; i++) {
            const uint8_t r = exp[(11 * (112 + i)) % 255];
            uint8_t ng[33] = {};
            for (int k = 0; k <= deg; k++) { ng[k] ^= g[k]; ng[k + 1] ^= mul(g[k], r); }   // (g * x) + g * r, highest first
            deg++;
            std::memcpy(g, ng, sizeof g);
        }
        std::memcpy(gen, g, sizeof gen);
    }
    uint8_t mul(uint8_t a, uint8_t b) const { return a && b ? exp[log[a] + log[b]] : 0; }
    uint8_t div(uint8_t a, uint8_t b) const { return a ? exp[(log[a] - log[b] + 255) % 255] : 0; }
    uint8_t pw(int lg) const { lg %= 255; if (lg < 0) lg += 255; return exp[lg]; }
};
const Gf& gf() { static const Gf g; return g; }

uint8_t polyEval(const uint8_t* p, int deg, uint8_t x) {   // p[0] constant term
    const Gf& G = gf();
    uint8_t r = 0;
    for (int i = deg; i >= 0; i--) r = (uint8_t)(G.mul(r, x) ^ p[i]);
    return r;
}

int rsDecodeConv(uint8_t c[255]) {
    const Gf& G = gf();
    uint8_t S[32];
    bool any = false;
    for (int j = 0; j < 32; j++) {
        const uint8_t b = G.pw(11 * (112 + j));
        uint8_t s = 0;
        for (int i = 0; i < 255; i++) s = (uint8_t)(G.mul(s, b) ^ c[i]);
        S[j] = s; any |= s != 0;
    }
    if (!any) return 0;
    // Berlekamp - Massey
    uint8_t C[33] = {1}, B[33] = {1}, T[33];
    int L = 0, m = 1;
    uint8_t bb = 1;
    for (int n = 0; n < 32; n++) {
        uint8_t d = S[n];
        for (int i = 1; i <= L; i++) d ^= G.mul(C[i], S[n - i]);
        if (!d) { m++; continue; }
        const uint8_t coef = G.div(d, bb);
        std::memcpy(T, C, sizeof T);
        for (int i = 0; i + m <= 32; i++) C[i + m] ^= G.mul(coef, B[i]);
        if (2 * L <= n) { L = n + 1 - L; std::memcpy(B, T, sizeof B); bb = d; m = 1; }
        else m++;
    }
    if (L > 16) return -1;
    // Omega = S * Lambda mod x^32
    uint8_t Om[32] = {};
    for (int i = 0; i < 32; i++)
        for (int k = 0; k <= std::min(i, L); k++) Om[i] ^= G.mul(C[k], S[i - k]);
    int found = 0;
    int pos[16];
    uint8_t val[16];
    for (int i = 0; i < 255 && found <= L; i++) {
        const int lx = (11 * (254 - i)) % 255;   // log of X for position i
        const uint8_t xinv = G.pw(-lx);
        if (polyEval(C, L, xinv)) continue;
        if (found == 16) return -1;
        uint8_t dl = 0;   // Lambda'(xinv): odd terms
        for (int k = 1; k <= L; k += 2) dl ^= G.mul(C[k], G.pw(G.log[xinv] * (k - 1)));
        if (!dl) return -1;
        const uint8_t om = polyEval(Om, 31, xinv);
        const uint8_t e = G.mul(G.pw(lx * (1 - 112)), G.div(om, dl));
        pos[found] = i; val[found] = e; found++;
    }
    if (found != L) return -1;
    for (int k = 0; k < found; k++) c[pos[k]] ^= val[k];
    return found;
}

// --- PN ------------------------------------------------------------------------------------------------------------------------------
const std::vector<uint8_t>& pnSeq() {
    static const std::vector<uint8_t> s = [] {
        std::vector<uint8_t> v(kCodedBytes);
        unsigned sr = 0xFF;
        for (auto& byte : v) {
            unsigned b = 0;
            for (int j = 0; j < 8; j++) {
                b = b << 1 | (sr & 1);
                const unsigned fb = (sr ^ sr >> 3 ^ sr >> 5 ^ sr >> 7) & 1;
                sr = sr >> 1 | fb << 7;
            }
            byte = (uint8_t)b;
        }
        return v;
    }();
    return s;
}

// --- JPEG pieces -------------------------------------------------------------------------------------------------------------------
const uint8_t kZz[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
                         35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
const uint8_t kQ50[64] = {16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
                          18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
const uint8_t kDcBits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
const uint8_t kDcVals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
const uint8_t kAcBits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
const uint8_t kAcVals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

struct Huff {
    int maxcode[17], mincode[17], valptr[17];
    std::vector<uint8_t> vals;
    uint16_t code[256] = {};
    uint8_t len[256] = {};
    Huff(const uint8_t* bits, const uint8_t* v, int nv) : vals(v, v + nv) {
        int c = 0, k = 0;
        for (int l = 1; l <= 16; l++) {
            const int n = bits[l - 1];
            valptr[l] = k; mincode[l] = c;
            for (int i = 0; i < n; i++, k++, c++) { code[v[k]] = (uint16_t)c; len[v[k]] = (uint8_t)l; }
            maxcode[l] = n ? c - 1 : -1;
            c <<= 1;
        }
    }
};
const Huff& dcHuff() { static const Huff h(kDcBits, kDcVals, 12); return h; }
const Huff& acHuff() { static const Huff h(kAcBits, kAcVals, 162); return h; }

struct BitReader {
    const uint8_t* p; size_t n, pos = 0;
    bool over = false;
    int bit() { if (pos >= n * 8) { over = true; return 1; } const int b = p[pos >> 3] >> (7 - (pos & 7)) & 1; pos++; return b; }
    int bits(int k) { int v = 0; for (int i = 0; i < k; i++) v = v << 1 | bit(); return v; }
    int decode(const Huff& h) {
        int c = 0;
        for (int l = 1; l <= 16; l++) {
            c = c << 1 | bit();
            if (over) return -1;
            if (h.maxcode[l] >= 0 && c <= h.maxcode[l] && c >= h.mincode[l]) return h.vals[(size_t)(h.valptr[l] + c - h.mincode[l])];
        }
        return -1;
    }
};
inline int extend(int v, int s) { return s && v < (1 << (s - 1)) ? v - (1 << s) + 1 : v; }

struct BitWriter {
    std::vector<uint8_t> out;
    unsigned acc = 0; int n = 0;
    void put(unsigned v, int k) { for (int i = k - 1; i >= 0; i--) { acc = acc << 1 | (v >> i & 1); if (++n == 8) { out.push_back((uint8_t)acc); acc = 0; n = 0; } } }
    void finish() { while (n) put(1, 1); }
};
inline int category(int v) { int a = std::abs(v), c = 0; while (a) { c++; a >>= 1; } return c; }
inline unsigned magBits(int v, int c) { return v >= 0 ? (unsigned)v : (unsigned)(v + (1 << c) - 1); }

const float* cosTab() {
    static float t[64];
    static bool init = false;
    if (!init) { for (int x = 0; x < 8; x++) for (int u = 0; u < 8; u++) t[x * 8 + u] = (float)((u ? 1.0 : std::sqrt(0.5)) * std::cos((2 * x + 1) * u * M_PI / 16) * 0.5); init = true; }
    return t;
}

void idct(const float F[64], uint8_t out[8][8]) {
    const float* c = cosTab();
    float tmp[64];
    for (int v = 0; v < 8; v++)        // rows of frequencies: transform u -> x
        for (int x = 0; x < 8; x++) { float s = 0; for (int u = 0; u < 8; u++) s += c[x * 8 + u] * F[v * 8 + u]; tmp[v * 8 + x] = s; }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            float s = 0;
            for (int v = 0; v < 8; v++) s += c[y * 8 + v] * tmp[v * 8 + x];
            const int p = (int)std::lround(s + 128);
            out[y][x] = (uint8_t)std::clamp(p, 0, 255);
        }
}

void fdct(const uint8_t in[8][8], float F[64]) {
    const float* c = cosTab();
    float tmp[64];
    for (int y = 0; y < 8; y++)
        for (int u = 0; u < 8; u++) { float s = 0; for (int x = 0; x < 8; x++) s += c[x * 8 + u] * ((float)in[y][x] - 128); tmp[y * 8 + u] = s; }
    for (int v = 0; v < 8; v++)
        for (int u = 0; u < 8; u++) { float s = 0; for (int y = 0; y < 8; y++) s += c[y * 8 + v] * tmp[y * 8 + u]; F[v * 8 + u] = s; }
}

} // namespace

// --- convolutional ------------------------------------------------------------------------------------------------------------------
void ConvEncoder::push(int bit, uint8_t& a, uint8_t& b) {
    sr = (uint8_t)(((sr << 1) | (bit & 1)) & 0x7F);
    a = (uint8_t)parity8(sr & kPolyA);
    b = (uint8_t)parity8(sr & kPolyB);
}

uint64_t encodedAsm() {
    ConvEncoder e;
    uint64_t v = 0;
    for (int i = 31; i >= 0; i--) { uint8_t a, b; e.push((int)(kAsm >> i & 1), a, b); v = v << 2 | (uint64_t)a << 1 | b; }
    return v;
}

namespace {
constexpr int kTb = 96, kEmit = 64;
struct Branch { uint8_t a[128], b[128]; Branch() { for (int r = 0; r < 128; r++) { a[r] = (uint8_t)parity8(r & kPolyA); b[r] = (uint8_t)parity8(r & kPolyB); } } };
const Branch& branch() { static const Branch b; return b; }
}

void Viterbi27::reset() { m_.fill(0); dec_.clear(); out_.clear(); steps_ = 0; }

void Viterbi27::push(int a, int b) {
    const Branch& br = branch();
    std::array<int32_t, 64> nm;
    uint64_t d = 0;
    for (int s = 0; s < 64; s++) {
        const int bit = s & 1;
        const int p0 = s >> 1, p1 = (s >> 1) | 32;
        const int r0 = (p0 << 1 | bit) & 0x7F, r1 = (p1 << 1 | bit) & 0x7F;
        const int32_t m0 = m_[(size_t)p0] + (br.a[r0] ? a : -a) + (br.b[r0] ? b : -b);
        const int32_t m1 = m_[(size_t)p1] + (br.a[r1] ? a : -a) + (br.b[r1] ? b : -b);
        if (m1 > m0) { nm[(size_t)s] = m1; d |= 1ull << s; } else nm[(size_t)s] = m0;
    }
    int32_t mx = nm[0];
    for (int32_t v : nm) mx = std::max(mx, v);
    if (mx > (1 << 28)) for (auto& v : nm) v -= mx;
    for (auto& v : nm) v = std::max(v, -(1 << 29));
    m_ = nm;
    dec_.push_back(d);
    steps_++;
    if ((int)dec_.size() >= kTb + kEmit) traceback(kEmit, false);
}

void Viterbi27::traceback(int emit, bool all) {
    int s = 0;
    for (int i = 1; i < 64; i++) if (m_[(size_t)i] > m_[(size_t)s]) s = i;
    const int n = (int)dec_.size();
    std::vector<uint8_t> bits((size_t)n);
    for (int t = n - 1; t >= 0; t--) {
        bits[(size_t)t] = (uint8_t)(s & 1);
        s = (s >> 1) | (int)((dec_[(size_t)t] >> s & 1) << 5);
    }
    const int k = all ? n : emit;
    out_.insert(out_.end(), bits.begin(), bits.begin() + k);
    dec_.erase(dec_.begin(), dec_.begin() + k);
}

void Viterbi27::flush() { if (!dec_.empty()) traceback(0, true); }

// --- RS ------------------------------------------------------------------------------------------------------------------------------
void rsEncode(const uint8_t data[223], uint8_t parity[32], bool dual) {
    const Gf& G = gf();
    uint8_t r[32] = {};
    for (int i = 0; i < 223; i++) {
        const uint8_t fb = (uint8_t)((dual ? G.tal1[data[i]] : data[i]) ^ r[0]);
        std::memmove(r, r + 1, 31); r[31] = 0;
        if (fb) for (int j = 0; j < 32; j++) r[j] ^= G.mul(fb, G.gen[j + 1]);
    }
    for (int j = 0; j < 32; j++) parity[j] = dual ? G.tal[r[j]] : r[j];
}

int rsDecode(uint8_t cw[255], bool dual) {
    if (!dual) return rsDecodeConv(cw);
    const Gf& G = gf();
    uint8_t c[255];
    for (int i = 0; i < 255; i++) c[i] = G.tal1[cw[i]];
    const int e = rsDecodeConv(c);
    if (e > 0) for (int i = 0; i < 255; i++) cw[i] = G.tal[c[i]];
    return e;
}

// --- frame ---------------------------------------------------------------------------------------------------------------------------
void pnXor(uint8_t* p, size_t n) {
    const auto& s = pnSeq();
    for (size_t i = 0; i < n; i++) p[i] ^= s[i % s.size()];
}

int decodeCadu(const uint8_t coded[kCodedBytes], uint8_t vcdu[kVcduBytes], bool dual) {
    uint8_t b[kCodedBytes];
    std::memcpy(b, coded, sizeof b);
    pnXor(b, sizeof b);
    int total = 0;
    bool bad = false;
    for (int k = 0; k < 4; k++) {
        uint8_t cw[255];
        for (int i = 0; i < 255; i++) cw[i] = b[i * 4 + k];
        const int e = rsDecode(cw, dual);
        if (e < 0) bad = true; else total += e;
        for (int i = 0; i < 223; i++) vcdu[i * 4 + k] = cw[i];
    }
    return bad ? -1 : total;
}

void encodeCadu(const uint8_t vcdu[kVcduBytes], uint8_t cadu[kCaduBytes], bool dual) {
    cadu[0] = (uint8_t)(kAsm >> 24); cadu[1] = (uint8_t)(kAsm >> 16); cadu[2] = (uint8_t)(kAsm >> 8); cadu[3] = (uint8_t)kAsm;
    uint8_t* b = cadu + 4;
    for (int k = 0; k < 4; k++) {
        uint8_t d[223], par[32];
        for (int i = 0; i < 223; i++) d[i] = vcdu[i * 4 + k];
        rsEncode(d, par, dual);
        for (int i = 0; i < 223; i++) b[i * 4 + k] = d[i];
        for (int i = 0; i < 32; i++) b[(223 + i) * 4 + k] = par[i];
    }
    pnXor(b, kCodedBytes);
}

// --- VCDU / packets ------------------------------------------------------------------------------------------------------------------
bool parseVcdu(const uint8_t v[kVcduBytes], Vcdu& o) {
    o.version = v[0] >> 6;
    o.scid = (v[0] & 0x3F) << 2 | v[1] >> 6;
    o.vcid = v[1] & 0x3F;
    o.counter = (uint32_t)v[2] << 16 | (uint32_t)v[3] << 8 | v[4];
    o.fhp = (v[8] & 7) << 8 | v[9];
    o.data = v + 10;
    return o.version == 1 && (o.fhp == 0x7FF || o.fhp < kMpduData);
}

void buildVcduHeader(uint8_t v[kVcduBytes], int scid, int vcid, uint32_t counter, int fhp) {
    v[0] = (uint8_t)(1 << 6 | (scid >> 2 & 0x3F));
    v[1] = (uint8_t)((scid & 3) << 6 | (vcid & 0x3F));
    v[2] = (uint8_t)(counter >> 16); v[3] = (uint8_t)(counter >> 8); v[4] = (uint8_t)counter;
    v[5] = 0; v[6] = 0; v[7] = 0;
    v[8] = (uint8_t)(fhp >> 8 & 7); v[9] = (uint8_t)fhp;
}

void PacketDemux::take(const uint8_t* p, size_t n) {
    buf_.insert(buf_.end(), p, p + n);
    size_t at = 0;
    while (buf_.size() - at >= 6) {
        const uint8_t* h = &buf_[at];
        if ((h[0] >> 5) != 0) { have_ = false; buf_.clear(); dropped++; return; }   // not a packet header: wait for the next pointer
        const size_t len = (size_t)((h[4] << 8) | h[5]) + 1 + 6;
        if (buf_.size() - at < len) break;
        const int apid = (h[0] & 7) << 8 | h[1];
        if (apid != 0x7FF) {
            Packet pk;
            pk.apid = apid;
            pk.seq = (h[2] & 0x3F) << 8 | h[3];
            pk.data.assign(h + 6, h + len);
            packets++;
            if (onPacket) onPacket(pk);
        }
        at += len;
    }
    buf_.erase(buf_.begin(), buf_.begin() + (std::ptrdiff_t)at);
}

void PacketDemux::push(const Vcdu& v) {
    if (lastCounter_ >= 0 && v.counter != (uint32_t)((lastCounter_ + 1) & 0xFFFFFF)) { if (have_) dropped++; have_ = false; buf_.clear(); }
    lastCounter_ = v.counter;
    if (v.fhp == 0x7FF) { if (have_) take(v.data, kMpduData); return; }
    if (have_) take(v.data, (size_t)v.fhp);
    if (!buf_.empty()) dropped++;
    buf_.clear();
    have_ = true;
    take(v.data + v.fhp, (size_t)(kMpduData - v.fhp));
}

void PacketMux::add(int apid, int seq, const std::vector<uint8_t>& u) {
    starts_.push_back(q_.size());
    const size_t len = u.size() - 1;
    q_.push_back((uint8_t)(1 << 3 | (apid >> 8 & 7)));
    q_.push_back((uint8_t)apid);
    q_.push_back((uint8_t)(3 << 6 | (seq >> 8 & 0x3F)));
    q_.push_back((uint8_t)seq);
    q_.push_back((uint8_t)(len >> 8)); q_.push_back((uint8_t)len);
    q_.insert(q_.end(), u.begin(), u.end());
}

void PacketMux::flush() {
    size_t pad = kMpduData - q_.size() % kMpduData;
    if (pad == (size_t)kMpduData && !q_.empty()) return;
    if (pad < 7) pad += kMpduData;
    add(0x7FF, 0, std::vector<uint8_t>(pad - 6, 0x55));
}

bool PacketMux::next(uint8_t v[kVcduBytes], int scid, int vcid) {
    if (q_.size() < (size_t)kMpduData) return false;
    int fhp = 0x7FF;
    for (size_t s : starts_) if (s < (size_t)kMpduData) { fhp = (int)s; break; }
    buildVcduHeader(v, scid, vcid, counter_++ & 0xFFFFFF, fhp);
    std::memcpy(v + 10, q_.data(), kMpduData);
    q_.erase(q_.begin(), q_.begin() + kMpduData);
    std::vector<size_t> ns;
    for (size_t s : starts_) if (s >= (size_t)kMpduData) ns.push_back(s - kMpduData);
    starts_ = ns;
    return true;
}

// --- picture rows --------------------------------------------------------------------------------------------------------------------
void StripRows::reset() {
    cur_ = -1; lastSeq_ = -1; rowStartMs_ = -1; rowMs_ = 0;
    std::fill(std::begin(lastPos_), std::end(lastPos_), -1);
}

int StripRows::place(int c, int seq, int k, uint32_t ms) {
    c = std::clamp(c, 0, kNumChannels - 1);
    if (cur_ < 0) { cur_ = 0; lastSeq_ = seq; rowStartMs_ = ms; lastPos_[c] = k; return 0; }
    constexpr int64_t kDay = 86400000;
    int64_t rd = ((int64_t)ms - rowStartMs_ + kDay) % kDay;   // time since the row started, forward across midnight
    if (rd > kDay / 2) rd -= kDay;
    const int d = (seq - lastSeq_) & 0x3FFF;
    int adv = 0;
    if (rowMs_ > 0 && rd >= 0 && rd < 30 * 60000) adv = (int)std::lround((double)rd / rowMs_);   // also a row whose first channel was lost
    else if (d > 43 && d < 8192) adv = d / 43;
    // before a row time is known: a later time stamp is a new row (all channels of a row carry its time). Without this, a pass that
    // started mid-row put the next row's first channel into the first row (found when the 3.9 line acquisition locked earlier)
    if (!adv && rowMs_ <= 0 && rd > 0 && rd < 20000) adv = 1;
    if (!adv && lastPos_[c] >= 0 && k <= lastPos_[c]) adv = 1;
    if (adv == 1 && d <= 43 && rd > 0 && rd < 20000) rowMs_ = rowMs_ > 0 ? 0.8 * rowMs_ + 0.2 * (double)rd : (double)rd;   // learn a row's time
    if (adv) { cur_ += adv; rowStartMs_ = ms; std::fill(std::begin(lastPos_), std::end(lastPos_), -1); }
    lastPos_[c] = k; lastSeq_ = seq;
    return cur_;
}

// --- image packets -------------------------------------------------------------------------------------------------------------------
void qTable(int q, int out[64]) {
    q = std::clamp(q, 1, 100);
    const int ratio = q < 50 ? 5000 / q : 200 - 2 * q;
    for (int i = 0; i < 64; i++) out[i] = std::max(1, ((int)kQ50[i] * ratio / 50 + 1) / 2);
}

bool decodeImagePacket(const std::vector<uint8_t>& u, McuStrip& o) {
    if (u.size() < 15) return false;
    o.day = (uint32_t)u[0] << 8 | u[1];
    o.ms = (uint32_t)u[2] << 24 | (uint32_t)u[3] << 16 | (uint32_t)u[4] << 8 | u[5];
    o.mcu = u[8];
    o.q = u[13];
    if (o.q == 0 || o.q > 100 || o.mcu % kMcuPerPacket || o.mcu >= kMcuPerLine) return false;
    int qt[64];
    qTable(o.q, qt);
    BitReader br{u.data() + 14, u.size() - 14};
    int dc = 0;
    for (int m = 0; m < kMcuPerPacket; m++) {
        int z[64] = {};
        const int cat = br.decode(dcHuff());
        if (cat < 0 || cat > 11) return false;
        dc += extend(br.bits(cat), cat);
        z[0] = dc;
        for (int k = 1; k < 64;) {
            const int rs = br.decode(acHuff());
            if (rs < 0) return false;
            if (rs == 0) break;
            const int run = rs >> 4, size = rs & 15;
            if (rs == 0xF0) { k += 16; continue; }
            k += run;
            if (k > 63 || size == 0) return false;
            z[k++] = extend(br.bits(size), size);
        }
        if (br.over) return false;
        float F[64];
        for (int i = 0; i < 64; i++) F[kZz[i]] = (float)(z[i] * qt[kZz[i]]);
        uint8_t blk[8][8];
        idct(F, blk);
        for (int y = 0; y < 8; y++) std::memcpy(&o.px[y][m * 8], blk[y], 8);
    }
    return true;
}

std::vector<uint8_t> encodeImagePacket(const uint8_t px[8][kMcuPerPacket * 8], int mcu, int q, uint32_t day, uint32_t ms) {
    std::vector<uint8_t> u = {(uint8_t)(day >> 8), (uint8_t)day, (uint8_t)(ms >> 24), (uint8_t)(ms >> 16), (uint8_t)(ms >> 8), (uint8_t)ms, 0, 0,
                              (uint8_t)mcu, 0, 0, 0xFF, 0xF0, (uint8_t)q};
    int qt[64];
    qTable(q, qt);
    BitWriter w;
    const Huff &D = dcHuff(), &A = acHuff();
    int prev = 0;
    for (int m = 0; m < kMcuPerPacket; m++) {
        uint8_t blk[8][8];
        for (int y = 0; y < 8; y++) std::memcpy(blk[y], &px[y][m * 8], 8);
        float F[64];
        fdct(blk, F);
        int z[64];
        for (int i = 0; i < 64; i++) z[i] = (int)std::lround(F[kZz[i]] / qt[kZz[i]]);
        z[0] = std::clamp(z[0], -2047, 2047);
        const int diff = z[0] - prev;
        prev = z[0];
        const int c = category(diff);
        w.put(D.code[c], D.len[c]);
        if (c) w.put(magBits(diff, c), c);
        int run = 0;
        int last = 63;
        while (last > 0 && z[last] == 0) last--;
        for (int k = 1; k <= last; k++) {
            const int v = std::clamp(z[k], -1023, 1023);
            if (!v) { run++; continue; }
            while (run >= 16) { w.put(A.code[0xF0], A.len[0xF0]); run -= 16; }
            const int s = category(v), sym = run << 4 | s;
            w.put(A.code[sym], A.len[sym]);
            w.put(magBits(v, s), s);
            run = 0;
        }
        if (last < 63) w.put(A.code[0], A.len[0]);
    }
    w.finish();
    u.insert(u.end(), w.out.begin(), w.out.end());
    return u;
}

} // namespace dect2::meteor
