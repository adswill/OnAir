// DAB channel coding: tables, CRCs, energy dispersal, the rate 1/4 convolutional code with puncturing (FIC and EEP sub-channels),
// and the Reed-Solomon code of DAB+. Conventions verified against a live Band III ensemble.
#include "dect2/dab.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace dect2 {
namespace dab {

// ------------------------------------------------------------------ carrier tables

const std::vector<int>& freqInterleaver() {
    static const std::vector<int> table = [] {
        std::vector<int> perm(kTu), out;
        perm[0] = 0;
        for (int i = 1; i < kTu; i++) perm[i] = (13 * perm[i - 1] + 511) % kTu;
        for (int i = 0; i < kTu; i++) {
            if (perm[i] == kTu / 2 || perm[i] < kTu / 2 - kCarriers / 2 || perm[i] > kTu / 2 + kCarriers / 2) continue;
            out.push_back(perm[i] - kTu / 2);
        }
        return out;
    }();
    return table;
}

const std::vector<cf32>& phaseReference() {
    static const std::vector<cf32> ref = [] {
        static const int8_t h[4][32] = {
            {0,2,0,0,0,0,1,1,2,0,0,0,2,2,1,1,0,2,0,0,0,0,1,1,2,0,0,0,2,2,1,1},
            {0,3,2,3,0,1,3,0,2,1,2,3,2,3,3,0,0,3,2,3,0,1,3,0,2,1,2,3,2,3,3,0},
            {0,0,0,2,0,2,1,3,2,2,0,2,2,0,1,3,0,0,0,2,0,2,1,3,2,2,0,2,2,0,1,3},
            {0,1,2,1,0,3,3,2,2,3,2,1,2,1,3,2,0,1,2,1,0,3,3,2,2,3,2,1,2,1,3,2}};
        struct Row { int kmin, kmax, i, n; };
        static const Row rows[] = {
            {-768,-737,0,1},{-736,-705,1,2},{-704,-673,2,0},{-672,-641,3,1},{-640,-609,0,3},{-608,-577,1,2},{-576,-545,2,2},{-544,-513,3,3},
            {-512,-481,0,2},{-480,-449,1,1},{-448,-417,2,2},{-416,-385,3,3},{-384,-353,0,1},{-352,-321,1,2},{-320,-289,2,3},{-288,-257,3,3},
            {-256,-225,0,2},{-224,-193,1,2},{-192,-161,2,2},{-160,-129,3,1},{-128,-97,0,1},{-96,-65,1,3},{-64,-33,2,1},{-32,-1,3,2},
            {1,32,0,3},{33,64,3,1},{65,96,2,1},{97,128,1,1},{129,160,0,2},{161,192,3,2},{193,224,2,1},{225,256,1,0},
            {257,288,0,2},{289,320,3,2},{321,352,2,3},{353,384,1,3},{385,416,0,0},{417,448,3,2},{449,480,2,1},{481,512,1,3},
            {513,544,0,3},{545,576,3,3},{577,608,2,3},{609,640,1,0},{641,672,0,3},{673,704,3,0},{705,736,2,1},{737,768,1,1}};
        static const cf32 rot[4] = {cf32(1, 0), cf32(0, 1), cf32(-1, 0), cf32(0, -1)};
        std::vector<cf32> x(kTu, cf32(0, 0));
        for (const Row& r : rows)
            for (int k = r.kmin; k <= r.kmax; k++) x[(size_t)((k + kTu) % kTu)] = rot[(h[r.i][k - r.kmin] + r.n) & 3];
        return x;
    }();
    return ref;
}

// ------------------------------------------------------------------ CRCs and scrambler

uint16_t crc16(const uint8_t* d, int n) {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < n; i++) {
        crc ^= (uint16_t)(d[i] << 8);
        for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return (uint16_t)~crc;
}

uint16_t fireCode(const uint8_t* d, int n) {
    uint16_t crc = 0;
    for (int i = 0; i < n; i++) {
        crc ^= (uint16_t)(d[i] << 8);
        for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x782F) : (uint16_t)(crc << 1);
    }
    return crc;
}

void descramble(uint8_t* bits, int n) {
    unsigned r = 0x1FF;
    for (int i = 0; i < n; i++) {
        const unsigned p = ((r >> 8) ^ (r >> 4)) & 1;
        bits[i] ^= (uint8_t)p;
        r = ((r << 1) | p) & 0x1FF;
    }
}

// ------------------------------------------------------------------ convolutional code

namespace {

struct Trellis {
    uint8_t out[64][2];   // 4 coded bits (bit k = generator k) for state s and input bit b
    Trellis() {
        static const int gens[4] = {0133, 0171, 0145, 0133};
        for (int s = 0; s < 64; s++)
            for (int b = 0; b < 2; b++) {
                const int reg = (b << 6) | s;
                int pat = 0;
                for (int k = 0; k < 4; k++) pat |= (__builtin_popcount(gens[k] & reg) & 1) << k;
                out[s][b] = (uint8_t)pat;
            }
    }
};
const Trellis& trellis() { static const Trellis t; return t; }

// puncturing vectors PI_1 .. PI_24 (32 positions = 8 groups of 4 code bits), and PI_X for the tail
const uint8_t* piVector(int i) {
    static const char* kStr[25] = {
        "", "11001000100010001000100010001000", "11001000100010001100100010001000", "11001000110010001100100010001000",
        "11001000110010001100100011001000", "11001100110010001100100011001000", "11001100110010001100110011001000",
        "11001100110011001100110011001000", "11001100110011001100110011001100", "11101100110011001100110011001100",
        "11101100110011001110110011001100", "11101100111011001110110011001100", "11101100111011001110110011101100",
        "11101110111011001110110011101100", "11101110111011001110111011101100", "11101110111011101110111011101100",
        "11101110111011101110111011101110", "11111110111011101110111011101110", "11111110111011101111111011101110",
        "11111110111111101111111011101110", "11111110111111101111111011111110", "11111111111111101111111011111110",
        "11111111111111101111111111111110", "11111111111111111111111111111110", "11111111111111111111111111111111"};
    static uint8_t table[25][32];
    static bool init = false;
    if (!init) {
        for (int k = 1; k <= 24; k++) for (int j = 0; j < 32; j++) table[k][j] = (uint8_t)(kStr[k][j] - '0');
        init = true;
    }
    return table[i];
}
const uint8_t kPiX[24] = {1,1,0,0, 1,1,0,0, 1,1,0,0, 1,1,0,0, 1,1,0,0, 1,1,0,0};

struct Block { int count, pi; };   // `count` blocks of 128 mother-code bits punctured with PI_`pi`

// Visits the mother-code positions that survive puncturing, in order; f(pos) returns false to stop
template <class F> bool forEachKept(const std::vector<Block>& blocks, int motherLen, F f) {
    int pos = 0;
    for (const Block& b : blocks)
        for (int c = 0; c < b.count; c++) {
            const uint8_t* v = piVector(b.pi);
            for (int rep = 0; rep < 4; rep++)
                for (int j = 0; j < 32; j++, pos++) if (v[j] && !f(pos)) return false;
        }
    for (int j = 0; j < 24; j++, pos++) if (kPiX[j] && !f(pos)) return false;
    return pos == motherLen;
}

std::vector<Block> ficBlocks() { return {{21, 16}, {3, 15}}; }

bool eepBlocks(int option, int level, int n, std::vector<Block>& out) {
    if (level < 0 || level > 3 || n < 1) return false;
    if (option == 0) {
        switch (level) {
        case 0: out = {{6 * n - 3, 24}, {3, 23}}; break;
        case 1: if (n < 2) return false; out = {{2 * n - 3, 14}, {4 * n + 3, 13}}; break;
        case 2: out = {{6 * n - 3, 8}, {3, 7}}; break;
        default: out = {{4 * n - 3, 3}, {2 * n + 3, 2}}; break;
        }
    } else {
        static const int pi1[4] = {10, 6, 4, 2};
        out = {{24 * n - 3, pi1[level]}, {3, pi1[level] - 1}};
    }
    return true;
}

} // namespace

void viterbiDecode(const int8_t* mother, int nbits, uint8_t* bits) {
    const Trellis& T = trellis();
    const int steps = nbits + 6;
    static thread_local std::vector<uint64_t> dec;
    dec.assign((size_t)steps, 0);
    int32_t pm[64], nm[64];
    for (int s = 0; s < 64; s++) pm[s] = -100000000;
    pm[0] = 0;
    // predecessor patterns of each next state
    uint8_t pat0[64], pat1[64];
    for (int ns = 0; ns < 64; ns++) {
        const int b = ns >> 5, ps0 = (ns & 31) << 1;
        pat0[ns] = T.out[ps0][b];
        pat1[ns] = T.out[ps0 | 1][b];
    }
    for (int t = 0; t < steps; t++) {
        const int s0 = mother[4 * t], s1 = mother[4 * t + 1], s2 = mother[4 * t + 2], s3 = mother[4 * t + 3];
        int bm[16];
        for (int p = 0; p < 16; p++) bm[p] = ((p & 1) ? s0 : -s0) + ((p & 2) ? s1 : -s1) + ((p & 4) ? s2 : -s2) + ((p & 8) ? s3 : -s3);
        uint64_t d = 0;
        for (int ns = 0; ns < 64; ns++) {
            const int ps0 = (ns & 31) << 1;
            const int a = pm[ps0] + bm[pat0[ns]], b = pm[ps0 | 1] + bm[pat1[ns]];
            if (b > a) { nm[ns] = b; d |= 1ull << ns; } else nm[ns] = a;
        }
        dec[(size_t)t] = d;
        std::memcpy(pm, nm, sizeof pm);
    }
    int state = 0;   // the tail forces the encoder back to state 0
    for (int t = steps - 1; t >= 0; t--) {
        if (t < nbits) bits[t] = (uint8_t)(state >> 5);
        state = ((state & 31) << 1) | (int)((dec[(size_t)t] >> state) & 1);
    }
}

void convEncode(const uint8_t* bits, int nbits, uint8_t* mother) {
    const Trellis& T = trellis();
    int s = 0;
    for (int t = 0; t < nbits + 6; t++) {
        const int b = t < nbits ? (bits[t] & 1) : 0;
        const int pat = T.out[s][b];
        for (int k = 0; k < 4; k++) mother[4 * t + k] = (uint8_t)((pat >> k) & 1);
        s = (((b << 6) | s) >> 1) & 63;
    }
}

bool ficDepuncture(const int8_t* rx, int8_t* mother) {
    std::memset(mother, 0, 3096);
    int k = 0;
    const bool ok = forEachKept(ficBlocks(), 3096, [&](int pos) { mother[pos] = rx[k++]; return k <= 2304; });
    return ok && k == 2304;
}

void ficPuncture(const uint8_t* mother, uint8_t* out) {
    int k = 0;
    forEachKept(ficBlocks(), 3096, [&](int pos) { out[k++] = mother[pos]; return true; });
}

bool eepGeometry(int size, int option, int level, int& n, int& bitrate, int& infoBits) {
    static const int unitA[4] = {12, 8, 6, 4}, unitB[4] = {27, 21, 18, 15};
    if (level < 0 || level > 3 || (option != 0 && option != 1)) return false;
    const int unit = option == 0 ? unitA[level] : unitB[level];
    if (size <= 0 || size % unit) return false;
    n = size / unit;
    std::vector<Block> b;
    if (!eepBlocks(option, level, n, b)) return false;
    bitrate = (option == 0 ? 8 : 32) * n;
    infoBits = bitrate * 24;
    return true;
}

bool eepDepuncture(const int8_t* rx, int size, int option, int level, int8_t* mother) {
    int n, br, info;
    std::vector<Block> blocks;
    if (!eepGeometry(size, option, level, n, br, info) || !eepBlocks(option, level, n, blocks)) return false;
    const int len = 4 * (info + 6), want = size * kCuBits;
    std::memset(mother, 0, (size_t)len);
    int k = 0;
    const bool ok = forEachKept(blocks, len, [&](int pos) { if (k >= want) return false; mother[pos] = rx[k++]; return true; });
    return ok && k == want;
}

bool eepPuncture(const uint8_t* mother, int size, int option, int level, uint8_t* out) {
    int n, br, info;
    std::vector<Block> blocks;
    if (!eepGeometry(size, option, level, n, br, info) || !eepBlocks(option, level, n, blocks)) return false;
    int k = 0;
    forEachKept(blocks, 4 * (info + 6), [&](int pos) { out[k++] = mother[pos]; return true; });
    return k == size * kCuBits;
}

// UEP (short form, EN 300 401 table 6): index -> sub-channel size in CUs, bitrate, protection level, L1..L4 and PI1..PI4. Every row
// satisfies 32 * (L1 + .. + L4) = 24 * bitrate and fits its size with fewer than 64 padding bits (checked by test_dab_realworld).
namespace {
struct UepRow { short size, bitrate, level, L[4], PI[4]; };
const UepRow kUep[64] = {
    {16, 32, 5, {3, 4, 17, 0}, {5, 3, 2, 0}}, {21, 32, 4, {3, 3, 18, 0}, {11, 6, 5, 0}}, {24, 32, 3, {3, 4, 14, 3}, {15, 9, 6, 8}},
    {29, 32, 2, {3, 4, 14, 3}, {22, 13, 8, 13}}, {35, 32, 1, {3, 5, 13, 3}, {24, 17, 12, 17}},
    {24, 48, 5, {4, 3, 26, 3}, {5, 4, 2, 3}}, {29, 48, 4, {3, 4, 26, 3}, {9, 6, 4, 6}}, {35, 48, 3, {3, 4, 26, 3}, {15, 10, 6, 9}},
    {42, 48, 2, {3, 4, 26, 3}, {24, 14, 8, 15}}, {52, 48, 1, {3, 5, 25, 3}, {24, 18, 13, 18}},
    {29, 56, 5, {6, 10, 23, 3}, {5, 4, 2, 3}}, {35, 56, 4, {6, 10, 23, 3}, {9, 6, 4, 5}}, {42, 56, 3, {6, 12, 21, 3}, {16, 7, 6, 9}},
    {52, 56, 2, {6, 10, 23, 3}, {23, 13, 8, 13}},
    {32, 64, 5, {6, 9, 31, 2}, {5, 3, 2, 3}}, {42, 64, 4, {6, 9, 33, 0}, {11, 6, 5, 0}}, {48, 64, 3, {6, 12, 27, 3}, {16, 8, 6, 9}},
    {58, 64, 2, {6, 10, 29, 3}, {23, 13, 8, 13}}, {70, 64, 1, {6, 11, 28, 3}, {24, 18, 12, 18}},
    {40, 80, 5, {6, 10, 41, 3}, {6, 3, 2, 3}}, {52, 80, 4, {6, 10, 41, 3}, {11, 6, 5, 6}}, {58, 80, 3, {6, 11, 40, 3}, {16, 8, 6, 7}},
    {70, 80, 2, {6, 10, 41, 3}, {23, 13, 8, 13}}, {84, 80, 1, {6, 10, 41, 3}, {24, 17, 12, 18}},
    {48, 96, 5, {7, 9, 53, 3}, {5, 4, 2, 4}}, {58, 96, 4, {7, 10, 52, 3}, {9, 6, 4, 6}}, {70, 96, 3, {6, 12, 51, 3}, {16, 9, 6, 10}},
    {84, 96, 2, {6, 10, 53, 3}, {22, 12, 9, 12}}, {104, 96, 1, {6, 13, 50, 3}, {24, 18, 13, 19}},
    {58, 112, 5, {14, 17, 50, 3}, {5, 4, 2, 5}}, {70, 112, 4, {11, 21, 49, 3}, {9, 6, 4, 8}}, {84, 112, 3, {11, 23, 47, 3}, {16, 8, 6, 9}},
    {104, 112, 2, {11, 21, 49, 3}, {23, 12, 9, 14}},
    {64, 128, 5, {12, 19, 62, 3}, {5, 3, 2, 4}}, {84, 128, 4, {11, 21, 61, 3}, {11, 6, 5, 7}}, {96, 128, 3, {11, 22, 60, 3}, {16, 9, 6, 10}},
    {116, 128, 2, {11, 21, 61, 3}, {22, 12, 9, 14}}, {140, 128, 1, {11, 20, 62, 3}, {24, 17, 13, 19}},
    {80, 160, 5, {11, 19, 87, 3}, {5, 4, 2, 4}}, {104, 160, 4, {11, 23, 83, 3}, {11, 6, 5, 9}}, {116, 160, 3, {11, 24, 82, 3}, {16, 8, 6, 11}},
    {140, 160, 2, {11, 21, 85, 3}, {22, 11, 9, 13}}, {168, 160, 1, {11, 22, 84, 3}, {24, 18, 12, 19}},
    {96, 192, 5, {11, 20, 110, 3}, {6, 4, 2, 5}}, {116, 192, 4, {11, 22, 108, 3}, {10, 6, 4, 9}}, {140, 192, 3, {11, 24, 106, 3}, {16, 10, 6, 11}},
    {168, 192, 2, {11, 20, 110, 3}, {22, 13, 9, 13}}, {208, 192, 1, {11, 21, 109, 3}, {24, 20, 13, 24}},
    {116, 224, 5, {12, 22, 131, 3}, {8, 6, 2, 6}}, {140, 224, 4, {12, 26, 127, 3}, {12, 8, 4, 11}}, {168, 224, 3, {11, 20, 134, 3}, {16, 10, 7, 9}},
    {208, 224, 2, {11, 22, 132, 3}, {24, 16, 10, 15}}, {232, 224, 1, {11, 24, 130, 3}, {24, 20, 12, 20}},
    {128, 256, 5, {11, 24, 154, 3}, {6, 5, 2, 5}}, {168, 256, 4, {11, 24, 154, 3}, {12, 9, 5, 10}}, {192, 256, 3, {11, 27, 151, 3}, {16, 10, 7, 10}},
    {232, 256, 2, {11, 22, 156, 3}, {24, 14, 10, 13}}, {280, 256, 1, {11, 26, 152, 3}, {24, 19, 14, 18}},
    {160, 320, 5, {11, 26, 200, 3}, {8, 5, 2, 6}}, {208, 320, 4, {11, 25, 201, 3}, {13, 9, 5, 10}}, {280, 320, 2, {11, 26, 200, 3}, {24, 17, 9, 17}},
    {192, 384, 5, {11, 27, 247, 3}, {8, 6, 2, 7}}, {280, 384, 3, {11, 24, 250, 3}, {16, 9, 7, 10}}, {416, 384, 1, {12, 28, 245, 3}, {24, 20, 14, 23}},
};
std::vector<Block> uepBlocks(const UepRow& r) {
    std::vector<Block> b;
    for (int i = 0; i < 4; i++) if (r.L[i] > 0) b.push_back({r.L[i], r.PI[i]});
    return b;
}
} // namespace

bool uepGeometry(int index, int& size, int& bitrate, int& level, int& infoBits) {
    if (index < 0 || index >= 64) return false;
    const UepRow& r = kUep[index];
    size = r.size; bitrate = r.bitrate; level = r.level; infoBits = r.bitrate * 24;
    return true;
}

bool uepDepuncture(const int8_t* rx, int index, int8_t* mother) {
    int size, br, lv, info;
    if (!uepGeometry(index, size, br, lv, info)) return false;
    const int len = 4 * (info + 6), have = size * kCuBits;
    std::memset(mother, 0, (size_t)len);
    int k = 0;
    const bool ok = forEachKept(uepBlocks(kUep[index]), len, [&](int pos) { if (k >= have) return false; mother[pos] = rx[k++]; return true; });
    return ok && have - k >= 0 && have - k < 64;   // the rest of the sub-channel is padding
}

bool uepPuncture(const uint8_t* mother, int index, uint8_t* out) {
    int size, br, lv, info;
    if (!uepGeometry(index, size, br, lv, info)) return false;
    const int have = size * kCuBits;
    int k = 0;
    const bool ok = forEachKept(uepBlocks(kUep[index]), 4 * (info + 6), [&](int pos) { if (k >= have) return false; out[k++] = mother[pos]; return true; });
    for (int i = k; i < have; i++) out[i] = 0;
    return ok && have - k < 64;
}

// ------------------------------------------------------------------ Reed-Solomon (120,110) over GF(256), generator roots alpha^0 .. alpha^9

namespace {
struct Gf {
    uint8_t exp[512];
    int log[256];
    uint8_t gen[11];   // generator polynomial, highest power first
    Gf() {
        int x = 1;
        for (int i = 0; i < 255; i++) { exp[i] = (uint8_t)x; log[x] = i; x <<= 1; if (x & 0x100) x ^= 0x11d; }
        for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
        log[0] = 0;
        int g[11] = {1};
        int deg = 0;
        for (int r = 0; r < 10; r++) {   // multiply by (x + alpha^r)
            for (int i = deg + 1; i >= 1; i--) g[i] = g[i - 1] ^ mul(g[i], exp[r]);
            g[0] = mul(g[0], exp[r]);
            deg++;
        }
        // g[] now holds lowest power first
        for (int i = 0; i <= 10; i++) gen[i] = (uint8_t)g[10 - i];
    }
    int mul(int a, int b) const { return (a && b) ? exp[log[a] + log[b]] : 0; }
    int inv(int a) const { return exp[255 - log[a]]; }
};
const Gf& gf() { static const Gf g; return g; }
} // namespace

void rsEncode120(const uint8_t* data, uint8_t* cw) {
    const Gf& G = gf();
    uint8_t rem[10] = {0};
    for (int i = 0; i < 110; i++) {
        cw[i] = data[i];
        const int fb = data[i] ^ rem[0];
        for (int j = 0; j < 9; j++) rem[j] = (uint8_t)(rem[j + 1] ^ G.mul(fb, G.gen[j + 1]));
        rem[9] = (uint8_t)G.mul(fb, G.gen[10]);
    }
    for (int j = 0; j < 10; j++) cw[110 + j] = rem[j];
}

int rsDecode120(uint8_t* cw) {
    const Gf& G = gf();
    const int n = 120, nr = 10;
    int syn[10];
    bool any = false;
    for (int i = 0; i < nr; i++) {
        int s = 0;
        for (int j = 0; j < n; j++) s = G.mul(s, G.exp[i]) ^ cw[j];
        syn[i] = s;
        any |= s != 0;
    }
    if (!any) return 0;
    // Berlekamp-Massey
    int C[11] = {1}, B[11] = {1}, L = 0, m = 1, b = 1;
    for (int k = 0; k < nr; k++) {
        int d = syn[k];
        for (int i = 1; i <= L; i++) d ^= G.mul(C[i], syn[k - i]);
        if (d == 0) { m++; continue; }
        int T[11];
        std::memcpy(T, C, sizeof T);
        const int coef = G.mul(d, G.inv(b));
        for (int i = 0; i + m <= nr; i++) C[i + m] ^= G.mul(coef, B[i]);
        if (2 * L <= k) { L = k + 1 - L; std::memcpy(B, T, sizeof B); b = d; m = 1; } else m++;
    }
    if (L > 5) return -1;
    // Chien search over the 120 positions (power of alpha = n - 1 - position)
    int pos[10], np = 0;
    for (int j = 0; j < n; j++) {
        const int p = n - 1 - j;
        const int xinv = (255 - p) % 255;   // log of alpha^-p
        int v = 0;
        for (int i = 0; i <= L; i++) v ^= G.mul(C[i], G.exp[(xinv * i) % 255]);
        if (v == 0) { if (np < 10) pos[np] = j; np++; }
    }
    if (np != L) return -1;
    int omega[10] = {0};
    for (int i = 0; i < nr; i++) for (int j = 0; j <= i; j++) omega[i] ^= G.mul(syn[j], C[i - j]);
    uint8_t out[120];
    std::memcpy(out, cw, sizeof out);
    for (int e = 0; e < np; e++) {
        const int p = n - 1 - pos[e];
        const int xinv = (255 - p) % 255;
        int num = 0, den = 0;
        for (int i = 0; i < nr; i++) num ^= G.mul(omega[i], G.exp[(xinv * i) % 255]);
        for (int i = 1; i <= L; i += 2) den ^= G.mul(C[i], G.exp[(xinv * (i - 1)) % 255]);
        if (den == 0) return -1;
        out[pos[e]] ^= (uint8_t)G.mul(G.mul(num, G.inv(den)), G.exp[p % 255]);
    }
    for (int i = 0; i < nr; i++) {   // verify
        int s = 0;
        for (int j = 0; j < n; j++) s = G.mul(s, G.exp[i]) ^ out[j];
        if (s) return -1;
    }
    std::memcpy(cw, out, sizeof out);
    return np;
}

} // namespace dab
} // namespace dect2
