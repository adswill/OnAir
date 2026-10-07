// DAB test transmitter: the channel coding primitives, written from EN 300 401 (clauses 10, 11) and TS 102 563 without looking at the receiver's
// dab_fec.cpp, so that the two can be compared by a test.
#include "dect2/dab_gen.h"
#include <cstring>

namespace dect2 {
namespace dabgen {

// ---------------------------------------------------------------- energy dispersal (clause 10.1)
// PRBS of x^9 + x^5 + 1 with all stages at 1: p(n) = p(n-5) xor p(n-9), the bits before the first one count as 1. Table 12: 0000 0111 1011 1110 ...
void prbs(uint8_t* bits, int n) {
    for (int i = 0; i < n; i++) {
        const uint8_t a = i >= 5 ? bits[i - 5] : 1, b = i >= 9 ? bits[i - 9] : 1;
        bits[i] = (uint8_t)(a ^ b);
    }
}

void scramble(uint8_t* bits, int n) {
    // the sequence has period 511; generate it once
    static const std::vector<uint8_t> seq = [] { std::vector<uint8_t> s(511); prbs(s.data(), 511); return s; }();
    for (int i = 0; i < n; i++) bits[i] ^= seq[(size_t)(i % 511)];
}

// ---------------------------------------------------------------- CRCs
uint16_t crc16(const uint8_t* d, int n) {
    unsigned reg = 0xFFFF;
    for (int i = 0; i < n; i++)
        for (int b = 7; b >= 0; b--) {
            const unsigned in = (d[i] >> b) & 1u, fb = ((reg >> 15) & 1u) ^ in;
            reg = (reg << 1) & 0xFFFFu;
            if (fb) reg ^= 0x1021u;
        }
    return (uint16_t)(~reg & 0xFFFFu);
}

uint16_t fireCode(const uint8_t* d, int n) {
    // g(x) = (x^11 + 1)(x^5 + x^3 + x^2 + x + 1), expanded: bits 14 13 12 11 5 3 2 1 0 below x^16
    unsigned reg = 0;
    const unsigned low = (1u << 14) | (1u << 13) | (1u << 12) | (1u << 11) | (1u << 5) | (1u << 3) | (1u << 2) | (1u << 1) | 1u;
    for (int i = 0; i < n; i++)
        for (int b = 7; b >= 0; b--) {
            const unsigned in = (d[i] >> b) & 1u, fb = ((reg >> 15) & 1u) ^ in;
            reg = (reg << 1) & 0xFFFFu;
            if (fb) reg ^= low;
        }
    return (uint16_t)reg;
}

// ---------------------------------------------------------------- Reed-Solomon (120,110): g(x) = prod_{i=0..9} (x + alpha^i) over GF(2^8), p(x) = x^8+x^4+x^3+x^2+1
namespace {
struct Gf256 {
    uint8_t exp[255];
    int log[256];
    uint8_t g[11];     // g[i] = coefficient of x^i
    Gf256() {
        int x = 1;
        for (int i = 0; i < 255; i++) { exp[i] = (uint8_t)x; log[x] = i; x <<= 1; if (x & 0x100) x ^= 0x11D; }
        log[0] = 0;
        std::memset(g, 0, sizeof g);
        g[0] = 1;
        int deg = 0;
        for (int r = 0; r < 10; r++) {     // multiply by (x + alpha^r)
            const uint8_t a = exp[r];
            for (int i = deg + 1; i >= 0; i--) g[i] = (uint8_t)((i ? g[i - 1] : 0) ^ mul(g[i], a));
            deg++;
        }
    }
    uint8_t mul(int a, int b) const { return (a && b) ? exp[(log[a] + log[b]) % 255] : 0; }
};
const Gf256& gf() { static const Gf256 f; return f; }
} // namespace

void rsEncode(const uint8_t* data, uint8_t* cw) {
    const Gf256& G = gf();
    // remainder of data(x) * x^10 modulo g(x); data[0] is the highest power
    uint8_t rem[10] = {0};      // rem[j] = coefficient of x^(9-j)
    for (int i = 0; i < 110; i++) {
        cw[i] = data[i];
        const uint8_t fb = (uint8_t)(data[i] ^ rem[0]);
        for (int j = 0; j < 9; j++) rem[j] = (uint8_t)(rem[j + 1] ^ G.mul(fb, G.g[9 - j]));
        rem[9] = G.mul(fb, G.g[0]);
    }
    for (int j = 0; j < 10; j++) cw[110 + j] = rem[j];
}

// ---------------------------------------------------------------- convolutional code (clause 11.1.1)
void convEncode(const uint8_t* bits, int n, uint8_t* mother) {
    // x_{k,i} = sum of a_{i-d} over the delays d of generator k (octal 133 171 145 133, the most significant bit is delay 0)
    static const int delays[4][7] = {{0, 2, 3, 5, 6, -1, -1}, {0, 1, 2, 3, 6, -1, -1}, {0, 1, 4, 6, -1, -1, -1}, {0, 2, 3, 5, 6, -1, -1}};
    for (int i = 0; i < n + 6; i++)
        for (int k = 0; k < 4; k++) {
            int v = 0;
            for (int t = 0; t < 7 && delays[k][t] >= 0; t++) {
                const int j = i - delays[k][t];
                if (j >= 0 && j < n) v ^= bits[j] & 1;      // a_i = 0 outside 0..n-1 (zero initial state and tail)
            }
            mother[4 * i + k] = (uint8_t)v;
        }
}

// ---------------------------------------------------------------- puncturing (clause 11.1.2, table 13)
const char* puncturingVector(int pi) {
    static const char* const t[24] = {
        "11001000100010001000100010001000", "11001000100010001100100010001000", "11001000110010001100100010001000", "11001000110010001100100011001000",
        "11001100110010001100100011001000", "11001100110010001100110011001000", "11001100110011001100110011001000", "11001100110011001100110011001100",
        "11101100110011001100110011001100", "11101100110011001110110011001100", "11101100111011001110110011001100", "11101100111011001110110011101100",
        "11101110111011001110110011101100", "11101110111011001110111011101100", "11101110111011101110111011101100", "11101110111011101110111011101110",
        "11111110111011101110111011101110", "11111110111011101111111011101110", "11111110111111101111111011101110", "11111110111111101111111011111110",
        "11111111111111101111111011111110", "11111111111111101111111111111110", "11111111111111111111111111111110", "11111111111111111111111111111111"};
    return pi >= 1 && pi <= 24 ? t[pi - 1] : nullptr;
}

namespace {
// 128 bit blocks of the mother codeword with their puncturing index, then the 24 tail bits
struct Run { int blocks, pi; };
void puncture(const uint8_t* mother, const Run* runs, int nRuns, uint8_t* out, int& count) {
    static const char* const tail = "110011001100110011001100";
    int pos = 0;
    count = 0;
    for (int r = 0; r < nRuns; r++)
        for (int b = 0; b < runs[r].blocks; b++) {
            const char* v = puncturingVector(runs[r].pi);
            for (int sub = 0; sub < 4; sub++)
                for (int i = 0; i < 32; i++, pos++) if (v[i] == '1') out[count++] = mother[pos];
        }
    for (int i = 0; i < 24; i++, pos++) if (tail[i] == '1') out[count++] = mother[pos];
}
} // namespace

void punctureFic(const uint8_t* mother, uint8_t* out) {
    const Run runs[2] = {{21, 16}, {3, 15}};       // clause 11.2.1: PI = 16 for the first 21 blocks, PI = 15 for the last 3
    int n = 0;
    puncture(mother, runs, 2, out, n);
}

int eepSize(int bitrate, int option, int level) {
    if (level < 0 || level > 3) return 0;
    if (option == 0) {
        static const int u[4] = {12, 8, 6, 4};
        if (bitrate <= 0 || bitrate % 8) return 0;
        const int n = bitrate / 8;
        if (level == 1 && n < 2) return 0;
        return u[level] * n;
    }
    if (option == 1) {
        static const int u[4] = {27, 21, 18, 15};
        if (bitrate <= 0 || bitrate % 32) return 0;
        return u[level] * (bitrate / 32);
    }
    return 0;
}

bool punctureEep(const uint8_t* mother, int bitrate, int option, int level, uint8_t* out) {
    const int size = eepSize(bitrate, option, level);
    if (!size) return false;
    Run runs[2];
    if (option == 0) {
        const int n = bitrate / 8;
        switch (level) {      // tables 18: L1 / PI1, L2 / PI2
        case 0: runs[0] = {6 * n - 3, 24}; runs[1] = {3, 23}; break;
        case 1: runs[0] = {2 * n - 3, 14}; runs[1] = {4 * n + 3, 13}; break;
        case 2: runs[0] = {6 * n - 3, 8}; runs[1] = {3, 7}; break;
        default: runs[0] = {4 * n - 3, 3}; runs[1] = {2 * n + 3, 2}; break;
        }
    } else {
        const int n = bitrate / 32;
        static const int pi1[4] = {10, 6, 4, 2};      // table 20: 1-B .. 4-B
        runs[0] = {24 * n - 3, pi1[level]}; runs[1] = {3, pi1[level] - 1};
    }
    int count = 0;
    puncture(mother, runs, 2, out, count);
    return count == size * 64;
}

} // namespace dabgen
} // namespace dect2
