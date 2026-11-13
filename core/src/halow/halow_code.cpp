// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi HaLow (802.11ah) channel coding pieces: CRC-4 of the SIG field, scrambler, pilot polarity, the BCC (rate 1/2, K = 7) with its Viterbi
// decoder and 2/3, 3/4, 5/6 puncturing, the BCC interleaver and the BPSK ... 256-QAM constellations (halow_phy.h). IEEE 802.11-2020 clause 23.3.11
// (data field coding, the 11a / 11n / 11ac convolutional code), 17.3.5.7 (puncturing), 21.3.10.9 (QAM mapping).
#include "dect2/halow/halow_phy.h"
#include "dect2/common/bits.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2::halow {

namespace {
int parity(uint32_t v) { return dect2::bits::parity((uint32_t)v); }

// keep patterns of the rate 1/2 stream (A1 B1 A2 B2 ...): 2/3 keeps A1 B1 A2, 3/4 keeps A1 B1 A2 B3, 5/6 keeps A1 B1 A2 B3 A4 B5 (clause 17.3.5.7, 19.3.11.7)
const uint8_t* punctPattern(int num, int den, int& period) {
    static const uint8_t p23[4] = {1, 1, 1, 0};
    static const uint8_t p34[6] = {1, 1, 1, 0, 0, 1};
    static const uint8_t p56[10] = {1, 1, 1, 0, 0, 1, 1, 0, 0, 1};
    static const uint8_t p12[2] = {1, 1};
    if (num == 2 && den == 3) { period = 4; return p23; }
    if (num == 3 && den == 4) { period = 6; return p34; }
    if (num == 5 && den == 6) { period = 10; return p56; }
    period = 2; return p12;
}
}

// Clause 23.3.9.? SIG CRC: x^4 + x + 1, register preset to ones, the result sent inverted, most significant bit first.
uint8_t crc4(const uint8_t* bits, size_t n) {
    uint8_t c = 0xF;
    for (size_t i = 0; i < n; i++) {
        const uint8_t fb = (uint8_t)(((c >> 3) & 1) ^ (bits[i] & 1));
        c = (uint8_t)((c << 1) & 0xF);
        if (fb) c ^= 0x3;
    }
    return (uint8_t)(~c & 0xF);
}

// The data scrambler S(x) = x^7 + x^4 + 1 (clause 17.3.5.5), started from a 7 bit state.
std::vector<uint8_t> scramblerSeq(uint8_t seed, size_t n) {
    std::vector<uint8_t> o(n);
    uint8_t h[7];
    for (int i = 0; i < 7; i++) h[i] = (uint8_t)(seed >> i & 1);   // h[i] = the output i + 1 places back
    for (size_t i = 0; i < n; i++) {
        const uint8_t b = h[3] ^ h[6];
        o[i] = b;
        for (int j = 6; j > 0; j--) h[j] = h[j - 1];
        h[0] = b;
    }
    return o;
}

// The same sequence recovered from its own first 7 bits (the descrambler reads them from the zeroed SERVICE bits).
std::vector<uint8_t> scramblerFrom(const uint8_t* first7, size_t n) {
    std::vector<uint8_t> o(n);
    uint8_t h[7];
    for (int i = 0; i < 7; i++) h[i] = first7[6 - i];
    for (size_t i = 0; i < n; i++) {
        if (i < 7) { o[i] = first7[i]; continue; }
        const uint8_t b = h[3] ^ h[6];
        o[i] = b;
        for (int j = 6; j > 0; j--) h[j] = h[j - 1];
        h[0] = b;
    }
    return o;
}

// Pilot polarity p_n (clause 17.3.5.10): the scrambler sequence from the all-ones state, 0 -> +1, 1 -> -1, period 127.
int pilotPolarity(int n) {
    static const std::vector<uint8_t> s = scramblerSeq(0x7F, 127);
    return s[(size_t)(n % 127)] ? -1 : 1;
}

// The BCC encoder: K = 7, generators 133 and 171 octal (clause 17.3.5.6), output A then B for every input bit.
std::vector<uint8_t> convEncode(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> out;
    out.reserve(bits.size() * 2);
    uint32_t reg = 0;
    for (uint8_t b : bits) {
        reg = (reg >> 1) | (uint32_t)(b & 1) << 6;
        out.push_back((uint8_t)parity(reg & 0x5B));
        out.push_back((uint8_t)parity(reg & 0x79));
    }
    return out;
}

// Soft Viterbi decoder of that code; the tail brings the encoder back to state 0. llr = 0 marks an erased (punctured) bit.
std::vector<uint8_t> viterbi(const std::vector<float>& llr) {
    const size_t n = llr.size() / 2;
    std::vector<uint8_t> dec(n * 64);
    float pm[64], nm[64];
    for (int s = 0; s < 64; s++) pm[s] = s == 0 ? 0.f : -1e9f;
    static const struct Tab {
        uint8_t o0[2][64], o1[2][64];   // output bits for [input][state]
        Tab() { for (int b = 0; b < 2; b++) for (int s = 0; s < 64; s++) { const uint32_t r = (uint32_t)(b << 6 | s); o0[b][s] = (uint8_t)parity(r & 0x5B); o1[b][s] = (uint8_t)parity(r & 0x79); } }
    } tab;
    for (size_t i = 0; i < n; i++) {
        const float l0 = llr[2 * i], l1 = llr[2 * i + 1];
        for (int ns = 0; ns < 64; ns++) {
            const int b = ns >> 5;
            float best = -1e30f; int bd = 0;
            for (int d = 0; d < 2; d++) {
                const int s = (ns & 0x1F) << 1 | d;
                const float m = pm[s] + (tab.o0[b][s] ? l0 : -l0) + (tab.o1[b][s] ? l1 : -l1);
                if (m > best) { best = m; bd = d; }
            }
            nm[ns] = best;
            dec[i * 64 + (size_t)ns] = (uint8_t)bd;
        }
        float mx = nm[0];
        for (int s = 1; s < 64; s++) mx = std::max(mx, nm[s]);
        for (int s = 0; s < 64; s++) pm[s] = nm[s] - mx;
    }
    std::vector<uint8_t> out(n);
    int s = 0;
    for (size_t i = n; i-- > 0;) {
        out[i] = (uint8_t)(s >> 5);
        s = (s & 0x1F) << 1 | dec[i * 64 + (size_t)s];
    }
    return out;
}

// Both interleaver permutations for one spatial stream (clause 23.3.11.8 = 21.3.10.8): i = Nrow (k mod Ncol) + floor(k / Ncol),
// j = s floor(i / s) + (i + Ncbps - floor(Ncol i / Ncbps)) mod s, s = max(1, Nbpscs / 2).
int interleaveJ(int ncol, int nrow, int ncbps, int nbpscs, int k) {
    const int i = nrow * (k % ncol) + k / ncol;
    const int s = std::max(1, nbpscs / 2);
    return s * (i / s) + (i + ncbps - ncol * i / ncbps) % s;
}

// Rate 2/3, 3/4 or 5/6 from the rate 1/2 stream.
std::vector<uint8_t> puncture(const std::vector<uint8_t>& coded, int num, int den) {
    int period = 2;
    const uint8_t* p = punctPattern(num, den, period);
    std::vector<uint8_t> o;
    o.reserve(coded.size());
    for (size_t i = 0; i < coded.size(); i++) if (p[i % (size_t)period]) o.push_back(coded[i]);
    return o;
}

// Puts the erasures back so the rate 1/2 Viterbi decoder can run (halfRateLen = coded bits of the mother code).
std::vector<float> depuncture(const std::vector<float>& llr, int num, int den, size_t halfRateLen) {
    int period = 2;
    const uint8_t* p = punctPattern(num, den, period);
    std::vector<float> o(halfRateLen, 0.f);
    size_t at = 0;
    for (size_t i = 0; i < halfRateLen && at < llr.size(); i++) if (p[i % (size_t)period]) o[i] = llr[at++];
    return o;
}

namespace {
// Axis of a square QAM with nb/2 bits per axis (BPSK: one axis of one bit): the level of bits g (most significant first) is 2 m - (L - 1) with g the Gray code of m.
int axisLevel(uint32_t g, int na) { const int L = 1 << na; return 2 * (int)dect2::bits::grayInverse(g) - (L - 1); }
float qamNorm(int nb) { switch (nb) { case 1: return 1.f; case 2: return 1.f / std::sqrt(2.f); case 4: return 1.f / std::sqrt(10.f); case 6: return 1.f / std::sqrt(42.f); default: return 1.f / std::sqrt(170.f); } }
}

// One tone: the first nb/2 bits give I, the next nb/2 bits give Q (BPSK: one bit on I).
cf32 mapQam(const uint8_t* b, int nb) {
    if (nb <= 1) return cf32(b[0] ? 1.f : -1.f, 0.f);
    const int na = nb / 2;
    uint32_t gi = 0, gq = 0;
    for (int i = 0; i < na; i++) { gi = gi << 1 | (b[i] & 1u); gq = gq << 1 | (b[na + i] & 1u); }
    const float a = qamNorm(nb);
    return cf32((float)axisLevel(gi, na) * a, (float)axisLevel(gq, na) * a);
}

// Max-log soft bits of one tone: the distance to the nearest level with the bit 0 and with the bit 1, per axis (rectangular Gray mapping).
void demapQam(cf32 z, float nvar, int nb, float* llr) {
    const float a = qamNorm(nb);
    const float nv = std::max(nvar / (a * a), 1e-9f);
    const int na = nb <= 1 ? 1 : nb / 2;
    const int L = 1 << na;
    for (int axis = 0; axis < (nb <= 1 ? 1 : 2); axis++) {
        const float v = (axis == 0 ? z.real() : z.imag()) / a;
        float d0[4], d1[4];
        for (int i = 0; i < na; i++) d0[i] = d1[i] = 1e30f;
        for (int m = 0; m < L; m++) {
            const uint32_t g = dect2::bits::gray((uint32_t)m);
            const float lev = (float)(2 * m - (L - 1));
            const float d = (v - lev) * (v - lev);
            for (int i = 0; i < na; i++) {
                if (g >> (na - 1 - i) & 1) d1[i] = std::min(d1[i], d); else d0[i] = std::min(d0[i], d);
            }
        }
        for (int i = 0; i < na; i++) llr[axis * na + i] = (d0[i] - d1[i]) / (2.f * nv);
    }
}

} // namespace dect2::halow
