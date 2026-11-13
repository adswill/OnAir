// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRA speech traffic channel decoding from ETSI EN 300 395-2 V1.3.1 clause 5.5, see tetra_traffic.h. Ambiguities of the printed text are marked AMBIGUITY.
#include "dect2/tetra/tetra_traffic.h"
#include <atomic>
#include <cstring>

namespace dect2::tetra {

namespace {

// Table 5: the speech frame bit (B-number of table 4) behind each pair of type-2 bits. Type-2 bit 2p+1 is B(kT5[p]) of frame A and 2p+2 the same bit of
// frame B. Pairs 0..50 are class 0 (type-2 bits 1..102), 51..106 class 1 (103..214), 107..136 class 2 (215..274).
const uint8_t kT5[137] = {
    35, 36, 37, 38, 39, 40, 41, 42, 43, 47, 48, 56, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 74, 75, 83, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97,
    101, 102, 110, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 128, 129, 137,
    58, 85, 112, 54, 81, 108, 135, 50, 77, 104, 131, 45, 72, 99, 126, 55, 82, 109, 136, 5, 13, 34, 8, 16, 17, 22, 23, 24, 25, 26, 6, 14, 7, 15,
    60, 87, 114, 46, 73, 100, 127, 44, 71, 98, 125, 33, 49, 76, 103, 130, 59, 86, 113, 57, 84, 111,
    18, 19, 20, 21, 31, 32, 53, 80, 107, 134, 1, 2, 3, 4, 9, 10, 11, 12, 27, 28, 29, 30, 52, 79, 106, 133, 51, 78, 105, 132};

constexpr int kC0 = 102, kC1 = 112, kC2 = 60;        // type-2 bits of classes 0, 1, 2 for both frames
constexpr int kParityAt = kC0 + kC1 + kC2;           // 274: the 8 parity bits b1..b8, then 4 tail bits
constexpr int kEncStart = kC0;                       // the convolutional encoder takes type-2 bits 103..286 (class 1, class 2, parity, tail)
constexpr int kEncBits = kTchSType2Bits - kC0;       // 184
constexpr int kMother = kEncBits * 3;                // 552

// Puncturing (5.5.2.1, 5.5.2.2): P(1..t) and Period. Class 1 (112 bits, 336 mother bits): rate 8/12; class 2 (72 bits, 216 mother bits): rate 8/18.
const int kP23[3] = {1, 2, 4};
const int kP818[9] = {1, 2, 3, 4, 5, 7, 8, 10, 11};
constexpr int kK3a = 168, kK3b = 162;                // 112 x 3/2 and 72 x 18/8
constexpr int kSegA = kC1 * 3;                       // mother bits of the class 1 part

// k of formula (49) for type-3 bit j (1-based): k = Period * ((j-1) div t) + P(j - t * ((j-1) div t)); 1-based index into the mother code bits
inline int punctK(int j, int period, int t, const int* P) {
    const int q = (j - 1) / t;
    return period * q + P[j - t * q - 1];
}

// The generator polynomials (48) as bit masks, bit j = coefficient of D^j: G1 = 1+D+D2+D3+D4, G2 = 1+D+D3+D4, G3 = 1+D2+D4
constexpr int kG[3] = {0x1F, 0x1B, 0x15};
inline int par(int v) { v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return v & 1; }

// CRC (5.5.1): G(X) = 1 + X^3 + X^7, F(X) = X^7 I(X) mod G(X), I(X) = C1(1) + C1(2) X + ... (C1(1) is the lowest degree term, as printed in (43)).
// c: the 60 class 2 type-1 bits in table 5 order. Returns f(0)..f(6) in bits 0..6.
int crc7(const uint8_t* c, int n) {
    int rem = 0;
    for (int i = n - 1; i >= 0; i--) {
        const int fb = ((rem >> 6) & 1) ^ (c[i] & 1);
        rem = (rem << 1) & 0x7F;
        if (fb) rem ^= 0x09;   // X^7 = 1 + X^3 (mod G)
    }
    return rem;
}

std::atomic<uint64_t> gOk{0}, gCrcBad{0};

} // namespace

void encodeTchS(const uint8_t* A, const uint8_t* B, uint8_t* type4) {
    uint8_t t2[kTchSType2Bits];
    for (int p = 0; p < 137; p++) { t2[2 * p] = A[kT5[p] - 1] & 1; t2[2 * p + 1] = B[kT5[p] - 1] & 1; }
    // parity: 7 CRC bits b1..b7 (AMBIGUITY: b_m taken as f(m-1), following (45) where C2(K1+1) = f(0)), b8 = overall parity of the class 2 bits and b1..b7
    const int f = crc7(t2 + kC0 + kC1, kC2);
    int all = 0;
    for (int i = 0; i < kC2; i++) all ^= t2[kC0 + kC1 + i];
    for (int m = 0; m < 7; m++) { t2[kParityAt + m] = (uint8_t)((f >> m) & 1); all ^= (f >> m) & 1; }
    t2[kParityAt + 7] = (uint8_t)all;
    for (int i = 0; i < 4; i++) t2[kParityAt + 8 + i] = 0;   // tail bits
    // mother code (47) over type-2 bits 103..286, zero initial state
    uint8_t v[kMother];
    int w = 0;   // bit j = C2(k-j)
    for (int k = 0; k < kEncBits; k++) {
        w = ((w << 1) | t2[kEncStart + k]) & 0x1F;
        for (int i = 0; i < 3; i++) v[3 * k + i] = (uint8_t)par(w & kG[i]);
    }
    // type-3: class 0 unprotected first, then the punctured class 1 part, then the punctured class 2 part
    uint8_t t3[kTchSBurstBits];
    std::memcpy(t3, t2, kC0);
    for (int j = 1; j <= kK3a; j++) t3[kC0 + j - 1] = v[punctK(j, 6, 3, kP23) - 1];
    for (int j = 1; j <= kK3b; j++) t3[kC0 + kK3a + j - 1] = v[kSegA + punctK(j, 12, 9, kP818) - 1];
    // matrix interleaving (52): C4(i*24 + j) = C3(j*18 + i). AMBIGUITY: the printed index ranges mix 0- and 1-based counting; read as the stated
    // transposition of the (24,18) matrix, 0-based: type4[i*24 + j] = type3[j*18 + i], i = 0..17, j = 0..23.
    for (int i = 0; i < 18; i++) for (int j = 0; j < 24; j++) type4[i * 24 + j] = t3[j * 18 + i];
}

std::vector<uint8_t> decodeTchS(const uint8_t* type4, size_t n, bool* crcOk) {
    if (crcOk) *crcOk = false;
    if (!type4 || n != (size_t)kTchSBurstBits) return {};
    uint8_t t3[kTchSBurstBits];
    for (int i = 0; i < 18; i++) for (int j = 0; j < 24; j++) t3[j * 18 + i] = type4[i * 24 + j] & 1;
    // depuncture: 2 = the mother bit was not sent
    uint8_t m[kMother];
    std::memset(m, 2, sizeof m);
    for (int j = 1; j <= kK3a; j++) m[punctK(j, 6, 3, kP23) - 1] = t3[kC0 + j - 1];
    for (int j = 1; j <= kK3b; j++) m[kSegA + punctK(j, 12, 9, kP818) - 1] = t3[kC0 + kK3a + j - 1];
    // Viterbi, 16 states, hard decisions, punctured bits cost nothing. State = the last four input bits (bit 0 = the most recent).
    constexpr int kInf = 1 << 28;
    int pm[16], nm[16];
    for (int s = 0; s < 16; s++) pm[s] = s == 0 ? 0 : kInf;
    uint8_t dbuf[kEncBits][16];   // per step and state: the oldest bit dropped from the predecessor state (identifies the predecessor)
    for (int k = 0; k < kEncBits; k++) {
        for (int s = 0; s < 16; s++) nm[s] = kInf;
        for (int s = 0; s < 16; s++) {
            if (pm[s] >= kInf) continue;
            for (int u = 0; u < 2; u++) {
                const int w = u | (s << 1);   // bit 0 = this input, bits 1..4 = state
                int cost = pm[s];
                for (int i = 0; i < 3; i++) {
                    const uint8_t r = m[3 * k + i];
                    if (r != 2 && (int)r != par(w & kG[i])) cost++;
                }
                const int ns = w & 0xF;
                if (cost < nm[ns]) { nm[ns] = cost; dbuf[k][ns] = (uint8_t)((w >> 4) & 1); }
            }
        }
        for (int s = 0; s < 16; s++) pm[s] = nm[s];
    }
    // The 4 tail bits are zero (5.5.1): the encoder ends in state 0. Trace back from there.
    uint8_t u[kEncBits];
    int s = 0;
    for (int k = kEncBits - 1; k >= 0; k--) {
        u[k] = (uint8_t)(s & 1);                       // the input at step k is bit 0 of the state it led to
        const int dropped = dbuf[k][s];
        s = (s >> 1) | (dropped << 3);                 // predecessor: bits 0..2 of this state shifted back, the dropped bit at the top
    }
    uint8_t t2[kTchSType2Bits];
    std::memcpy(t2, t3, kC0);
    std::memcpy(t2 + kEncStart, u, kEncBits);
    // CRC over the class 2 bits (5.5.1) and the overall parity b8
    const int f = crc7(t2 + kC0 + kC1, kC2);
    int all = 0;
    for (int i = 0; i < kC2; i++) all ^= t2[kC0 + kC1 + i];
    bool ok = true;
    for (int mm = 0; mm < 7; mm++) { if (t2[kParityAt + mm] != ((f >> mm) & 1)) ok = false; all ^= (f >> mm) & 1; }
    if (t2[kParityAt + 7] != (uint8_t)all) ok = false;
    if (crcOk) *crcOk = ok;
    if (!ok) { gCrcBad++; return {}; }
    gOk++;
    std::vector<uint8_t> out(2 * (size_t)kTchSFrameBits, 0);
    for (int p = 0; p < 137; p++) {
        out[(size_t)(kT5[p] - 1)] = t2[2 * p] & 1;
        out[(size_t)kTchSFrameBits + (size_t)(kT5[p] - 1)] = t2[2 * p + 1] & 1;
    }
    return out;
}

const uint8_t* tchSBitOrder() { return kT5; }

TrafficFrameDecoder makeTrafficFrameDecoder() {
    return [](const uint8_t* bits, size_t n) { return decodeTchS(bits, n, nullptr); };
}

uint64_t tchSBurstsOk() { return gOk.load(); }
uint64_t tchSBurstsCrcFailed() { return gCrcBad.load(); }

} // namespace dect2::tetra
