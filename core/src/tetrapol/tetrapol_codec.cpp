// TETRAPOL physical frames, PAS 0001-2 clause 6 (tetrapol_codec.h): voice and data frames of the 8 kbit/s GMSK channel.
#include "dect2/tetrapol/tetrapol_codec.h"
#include <algorithm>
#include <cstring>

namespace dect2 {
namespace tetrapol {

namespace {

constexpr int kPart1 = 26;   // protected information bits common to both frame types (d + 25)
constexpr int kPart2 = 50;   // second protected part of a data frame

// PAS 0001-2 6.1.3.1 / 6.2.3.1 (VHF, voice and data alike): k = 19 * p[j % 8] + (3 * (j / 8)) % 19, p the 3-bit reversal given there
int vhfIndex(int j) {
    static const int p[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    return 19 * p[j % 8] + (3 * (j / 8)) % 19;
}

// PAS 0001-2 6.1.4.1: UHF voice frame interleaving, the K vector of the text (checked entry by entry against PAS 0001-2 v3.0.0)
const uint8_t kUhfVoice[kDataBits] = {
    1, 77, 38, 114, 20, 96, 59, 135,   3, 79, 41, 117, 23, 99, 62, 138,   5, 81, 44, 120, 26, 102, 65, 141,
    8, 84, 47, 123, 29, 105, 68, 144,  11, 87, 50, 126, 32, 108, 71, 147, 14, 90, 53, 129, 35, 111, 74, 150,
    17, 93, 56, 132, 37, 113, 73, 4,   0, 76, 40, 119, 19, 95, 58, 137,   151, 80, 42, 115, 24, 100, 60, 133,
    12, 88, 48, 121, 30, 106, 66, 139, 18, 91, 51, 124, 28, 104, 67, 146, 10, 89, 52, 131, 34, 110, 70, 149,
    13, 97, 57, 130, 36, 112, 75, 148, 6, 82, 39, 116, 16, 92, 55, 134,   2, 78, 43, 122, 22, 98, 61, 140,
    9, 85, 45, 118, 27, 103, 63, 136,  15, 83, 46, 125, 25, 101, 64, 143, 7, 86, 49, 128, 31, 107, 69, 142,
    21, 94, 54, 127, 33, 109, 72, 145,
};
// PAS 0001-2 6.2.4.1: UHF data frame interleaving, the K vector of the text (checked entry by entry against PAS 0001-2 v3.0.0)
const uint8_t kUhfData[kDataBits] = {
    1, 77, 38, 114, 20, 96, 59, 135,   3, 79, 41, 117, 23, 99, 62, 138,   5, 81, 44, 120, 26, 102, 65, 141,
    8, 84, 47, 123, 29, 105, 68, 144,  11, 87, 50, 126, 32, 108, 71, 147, 14, 90, 53, 129, 35, 111, 74, 150,
    17, 93, 56, 132, 37, 112, 76, 148, 2, 88, 40, 115, 19, 97, 58, 133,   4, 75, 43, 118, 22, 100, 61, 136,
    7, 85, 46, 121, 25, 103, 64, 139,  10, 82, 49, 124, 28, 106, 67, 142, 13, 91, 52, 127, 31, 109, 73, 145,
    16, 94, 55, 130, 34, 113, 70, 151, 0, 80, 39, 116, 21, 95, 57, 134,   6, 78, 42, 119, 24, 98, 60, 137,
    9, 83, 45, 122, 27, 101, 63, 140,  12, 86, 48, 125, 30, 104, 66, 143, 15, 89, 51, 128, 33, 107, 69, 146,
    18, 92, 54, 131, 36, 110, 72, 149,
};

// PAS 0001-2 6.1.4.2 / 6.2.4.2 (UHF): bit j is precoded with bit j - 2 at the 47 PRE_COD positions (7, 10 .. 76, 83, 86 .. 149), with
// bit j - 1 elsewhere; e'0 = e0 + f7 and f7 = 0
int precodeLag(int j) {
    if (j >= 7 && j <= 76 && (j - 7) % 3 == 0) return 2;
    if (j >= 83 && j <= 149 && (j - 83) % 3 == 0) return 2;
    return 1;
}

// PAS 0001-2 6.1.5.1 / 6.2.5.1: s(k) = 1 for k = 0..6, s(k) = s(k-1) + s(k-7); S(SCR, k) = s(k + SCR), S(0, k) = 0
const uint8_t* scrambleTable() {
    static uint8_t s[127];
    static bool done = false;
    if (!done) {
        for (int k = 0; k < 7; k++) s[k] = 1;
        for (int k = 7; k < 127; k++) s[k] = (uint8_t)(s[k - 1] ^ s[k - 7]);
        done = true;
    }
    return s;
}

// The cyclic rate-1/2 code of the protected parts (PAS 0001-2 6.1.2 / 6.2.2)
void convEncode(const uint8_t* u, int n, uint8_t* c) {
    for (int i = 0; i < n; i++) {
        const uint8_t a = u[i], b = u[(i - 1 + n) % n], d = u[(i - 2 + n) % n];
        c[2 * i] = (uint8_t)(a ^ b ^ d);
        c[2 * i + 1] = (uint8_t)(a ^ d);
    }
}

// Tail-biting Viterbi (4 states, every start state tried; the path must end where it started). w: the weight of each coded bit (null: all 1,
// hard decisions). Returns the coded bits in error of the best path (hard count).
int convDecode(const uint8_t* c, int n, uint8_t* u, const float* w = nullptr) {
    float bestCost = 1e30f;
    uint8_t path[64][4];
    uint8_t best[64];
    constexpr float kInf = 1e29f;
    for (int s0 = 0; s0 < 4; s0++) {   // state = (u[i-1] << 1) | u[i-2]
        float met[4];
        for (int s = 0; s < 4; s++) met[s] = s == s0 ? 0.f : kInf;
        for (int i = 0; i < n; i++) {
            float nm[4] = {2 * kInf, 2 * kInf, 2 * kInf, 2 * kInf};
            const float w0 = w ? w[2 * i] : 1.f, w1 = w ? w[2 * i + 1] : 1.f;
            for (int s = 0; s < 4; s++) {
                if (met[s] >= kInf) continue;
                const int p1 = (s >> 1) & 1, p2 = s & 1;
                for (int b = 0; b < 2; b++) {
                    const int o0 = b ^ p1 ^ p2, o1 = b ^ p2;
                    const float m = met[s] + (o0 != c[2 * i] ? w0 : 0.f) + (o1 != c[2 * i + 1] ? w1 : 0.f);
                    const int ns = (b << 1) | p1;
                    if (m < nm[ns]) { nm[ns] = m; path[i][ns] = (uint8_t)s; }
                }
            }
            for (int s = 0; s < 4; s++) met[s] = nm[s];
        }
        if (met[s0] >= bestCost) continue;
        bestCost = met[s0];
        int s = s0;
        for (int i = n - 1; i >= 0; i--) {
            best[i] = (uint8_t)((s >> 1) & 1);
            s = path[i][s];
        }
    }
    memcpy(u, best, (size_t)n);
    // hard disagreements of the decoded path
    int err = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t a = u[i], b = u[(i - 1 + n) % n], d = u[(i - 2 + n) % n];
        err += (uint8_t)(a ^ b ^ d) != c[2 * i];
        err += (uint8_t)(a ^ d) != c[2 * i + 1];
    }
    return err;
}

void scramble(uint8_t* x, int scr) {
    if (scr <= 0) return;
    const uint8_t* s = scrambleTable();
    for (int k = 0; k < kDataBits; k++) x[k] ^= s[(k + scr) % 127];
}

void precode(uint8_t* x) {
    for (int j = 1; j < kDataBits; j++) x[j] ^= x[j - precodeLag(j)];
}
void unprecode(uint8_t* x) {
    for (int j = kDataBits - 1; j > 0; j--) x[j] ^= x[j - precodeLag(j)];
}

void finishFrame(const uint8_t coded[kDataBits], int band, int type, int scr, uint8_t bits160[kFrameBits]) {
    uint8_t x[kDataBits];
    for (int j = 0; j < kDataBits; j++) x[interleaveIndex(j, band, type)] = coded[j];
    if (band == kUhf) precode(x);
    scramble(x, scr);
    for (int i = 0; i < kHdrBits; i++) bits160[i] = (uint8_t)((kFrameHdr >> i) & 1);
    memcpy(bits160 + kHdrBits, x, (size_t)kDataBits);
}

} // namespace

uint8_t scrambleBit(int k, int scr) { return scr <= 0 ? 0 : scrambleTable()[(k + scr) % 127]; }

int interleaveIndex(int j, int band, int type) {
    if (band != kUhf) return vhfIndex(j);
    return type == kVoice ? kUhfVoice[j] : kUhfData[j];
}

// PAS 0001-2 6.2.1: (d0 D^73 + ... + d73) a multiple of I + D^2 + D^5
void crc5(const uint8_t* b, int n, uint8_t r[5]) {
    memset(r, 0, 5);
    for (int i = 0; i < n; i++) {
        const uint8_t inv = (uint8_t)((b[i] ^ r[0]) & 1);
        r[0] = r[1]; r[1] = r[2]; r[2] = (uint8_t)(r[3] ^ inv); r[3] = r[4]; r[4] = inv;
    }
}

// PAS 0001-2 6.1.1: (b'0 D^25 + ... + b'25) a multiple of I + D + D^3, then the three CRC bits complemented
void crc3(const uint8_t* b, int n, uint8_t r[3]) {
    memset(r, 0, 3);
    for (int i = 0; i < n; i++) {
        const uint8_t inv = (uint8_t)((b[i] ^ r[0]) & 1);
        r[0] = r[1]; r[1] = (uint8_t)(r[2] ^ inv); r[2] = inv;
    }
    for (int i = 0; i < 3; i++) r[i] ^= 1;
}

void encodeDataFrame(const DataFrame& in, int band, uint8_t bits160[kFrameBits]) {
    uint8_t u[kPart1 + kPart2] = {};
    u[0] = 1;
    for (int i = 0; i < kPayloadBits; i++) u[1 + i] = in.payload[i] & 1;
    u[67] = in.asb[0] & 1;
    u[68] = in.asb[1] & 1;
    crc5(u, 69, u + 69);   // u[74], u[75] stay zero
    uint8_t coded[kDataBits];
    convEncode(u, kPart1, coded);
    convEncode(u + kPart1, kPart2, coded + 2 * kPart1);
    finishFrame(coded, band, kData, in.scramble, bits160);
}

void encodeVoiceFrame(const VoiceFrame& in, int band, uint8_t bits160[kFrameBits]) {
    uint8_t u[kPart1] = {};
    u[0] = 0;
    for (int i = 0; i < 20; i++) u[1 + i] = in.codec[i] & 1;
    u[21] = in.asb[0] & 1;
    u[22] = in.asb[1] & 1;
    crc3(u, 23, u + 23);
    uint8_t coded[kDataBits];
    convEncode(u, kPart1, coded);
    for (int i = 0; i < 100; i++) coded[2 * kPart1 + i] = in.codec[20 + i] & 1;
    finishFrame(coded, band, kVoice, in.scramble, bits160);
}

bool decodeFrame(const uint8_t bits152[kDataBits], int band, int scr, Frame& out, int forceType, const float* rel152) {
    out = Frame();
    uint8_t x[kDataBits];
    memcpy(x, bits152, (size_t)kDataBits);
    scramble(x, scr);
    if (band == kUhf) unprecode(x);
    // reliability of each bit after the differential and the UHF precoding are undone: on UHF the two cancel (bit j is received bit j,
    // or j and j - 1 where the precoding used j - 2), on VHF bit j depends on every received bit up to j
    float rel[kDataBits];
    if (rel152) {
        float mean = 0;
        for (int j = 0; j < kDataBits; j++) mean += rel152[j];
        mean = mean / kDataBits + 1e-9f;
        float run = 1e30f;
        for (int j = 0; j < kDataBits; j++) {
            const float a = std::min(rel152[j] / mean, 2.f);   // outliers capped
            if (band == kUhf) rel[j] = precodeLag(j) == 2 && j > 0 ? std::min(a, std::min(rel152[j - 1] / mean, 2.f)) : a;
            else { run = std::min(run, a); rel[j] = run; }
        }
    }
    // the first 52 positions are interleaved alike in both frame types (the two tables agree there)
    uint8_t c[kDataBits];
    float w[kDataBits];
    for (int j = 0; j < kDataBits; j++) w[j] = 1.f;
    for (int j = 0; j < 2 * kPart1; j++) { const int k = interleaveIndex(j, band, kData); c[j] = x[k]; if (rel152) w[j] = rel[k]; }
    uint8_t u[kPart1 + kPart2] = {};
    const int e1 = convDecode(c, kPart1, u, rel152 ? w : nullptr);
    const int type = forceType >= 0 ? forceType : u[0];
    if (u[0] != type) return false;
    out.type = type;
    for (int j = 2 * kPart1; j < kDataBits; j++) { const int k = interleaveIndex(j, band, type); c[j] = x[k]; if (rel152) w[j] = rel[k]; }
    if (type == kVoice) {
        VoiceFrame& v = out.voice;
        v.scramble = scr;
        v.metric = e1;
        for (int i = 0; i < 20; i++) v.codec[i] = u[1 + i];
        for (int i = 0; i < 100; i++) v.codec[20 + i] = c[2 * kPart1 + i];
        v.asb[0] = u[21];
        v.asb[1] = u[22];
        uint8_t r[3];
        crc3(u, 23, r);
        v.ok = !memcmp(r, u + 23, 3);
        out.ok = v.ok;
        out.metric = e1;
        return out.ok;
    }
    const int e2 = convDecode(c + 2 * kPart1, kPart2, u + kPart1, rel152 ? w + 2 * kPart1 : nullptr);
    DataFrame& d = out.data;
    d.scramble = scr;
    d.metric = e1 + e2;
    for (int i = 0; i < kPayloadBits; i++) d.payload[i] = u[1 + i];
    d.asb[0] = u[67];
    d.asb[1] = u[68];
    uint8_t r[5];
    crc5(u, 69, r);
    d.ok = !memcmp(r, u + 69, 5) && !u[74] && !u[75];
    out.ok = d.ok;
    out.metric = d.metric;
    return out.ok;
}

uint8_t diffEncode(uint8_t* bits, int n, uint8_t prev) {
    for (int i = 0; i < n; i++) {
        const uint8_t x = bits[i] & 1;
        bits[i] = (uint8_t)(x ^ prev);
        prev = x;
    }
    return prev;
}

void frameBits152(const uint8_t raw160[kFrameBits], bool invert, uint8_t out152[kDataBits]) {
    uint8_t prev = 0;   // x[7]: the last header bit is 0
    const uint8_t f = invert ? 1 : 0;
    for (int i = 0; i < kDataBits; i++) {
        prev = (uint8_t)(((raw160[kHdrBits + i] ^ f) & 1) ^ prev);
        out152[i] = prev;
    }
}

int syncErrors(const uint8_t* raw, bool invert) {
    static const uint8_t pat[7] = {1, 0, 1, 0, 0, 1, 1};   // 0x46 after the differential, bits 1..7
    const uint8_t f = invert ? 1 : 0;
    int e = 0;
    for (int i = 0; i < 7; i++) e += ((raw[1 + i] ^ f) & 1) != pat[i];
    return e;
}

} // namespace tetrapol
} // namespace dect2
