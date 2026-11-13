// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DVB-C2 reference sequences, pilots, Reed-Muller headers, the preamble cell order, the time interleaver and the data slice geometry
// (EN 302 769 clauses 7.2.2, 8.2, 9.2 - 9.6). See dvbc2.h.
#include "dect2/dvbc2/dvbc2.h"
#include "dect2/t2/t2fec.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {
namespace dvbc2 {

// ---------------------------------------------------------------- PRBS (clause 9.2.1 / 9.2.2)
namespace {
// X^11 + X^2 + 1, all ones: 1 x 11, 0, 0, ... (the DVB-T sequence); period 2047
struct DataPrbs {
    uint8_t w[2047];
    DataPrbs() {
        unsigned sr = 0x7FF;
        for (int i = 0; i < 2047; i++) {
            w[i] = (uint8_t)(sr & 1);
            const unsigned fb = ((sr >> 0) ^ (sr >> 2)) & 1;   // taps 11 and 9 of the shift register (X^11 + X^2 + 1)
            sr = (sr >> 1) | (fb << 10);
        }
    }
};
// X^10 + X^3 + 1, all ones: 1 x 10, 0, 0, ...; period 1023
struct PilotPrbs {
    uint8_t w[1023];
    PilotPrbs() {
        unsigned sr = 0x3FF;
        for (int i = 0; i < 1023; i++) {
            w[i] = (uint8_t)(sr & 1);
            const unsigned fb = ((sr >> 0) ^ (sr >> 3)) & 1;   // taps 10 and 7 (X^10 + X^3 + 1)
            sr = (sr >> 1) | (fb << 9);
        }
    }
};
const DataPrbs& dataPrbs() { static DataPrbs p; return p; }
const PilotPrbs& pilotPrbsTab() { static PilotPrbs p; return p; }
inline int64_t floorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
inline int64_t floorMod(int64_t a, int64_t b) { return a - floorDiv(a, b) * b; }
} // namespace

uint8_t dataScramble(int64_t k) { return dataPrbs().w[floorMod(k, 2047)]; }
uint8_t pilotPrbs(int i) { return pilotPrbsTab().w[((i % 1023) + 1023) % 1023]; }

void ReferenceBlock::set(int64_t block) {
    if (block == block_) return;
    block_ = block;
    const int64_t k0 = block * kL1Carriers;
    uint8_t prev = 0;
    for (int j = 0; j < kL1Carriers / kPreamblePilotSpacing; j++) {
        const int64_t k = k0 + (int64_t)j * kPreamblePilotSpacing;
        const uint8_t wp = dataScramble(k) ^ pilotPrbs(j);   // w^P_k = w_k xor w'_i, i = (k mod K_L1) / D_P
        const uint8_t r = j == 0 ? wp : (uint8_t)(wp ^ prev);   // r_k = w^P_k (block start), w^P_k xor r_(k-6) otherwise
        r_[(size_t)j] = r;
        prev = r;
    }
}
uint8_t ReferenceBlock::bit(int64_t k) const {
    const int64_t j = floorMod(k, kL1Carriers) / kPreamblePilotSpacing;
    return r_[(size_t)j];
}
uint8_t referenceBit(int64_t k) {
    ReferenceBlock b;
    b.set(floorDiv(k, kL1Carriers));
    return b.bit(k);
}
float pilotValue(int64_t k, float amp) { return amp * (1.f - 2.f * referenceBit(k)); }

// ---------------------------------------------------------------- continual pilots (table 35)
const int kContinualPilots[30] = {96, 216, 306, 390, 450, 486, 780, 804, 924, 1026, 1224, 1422, 1554, 1620, 1680, 1902, 1956, 2016, 2142, 2220,
                                  2310, 2424, 2466, 2736, 3048, 3126, 3156, 3228, 3294, 3366};
const int kReservedS0[288] = {161, 243, 296, 405, 493, 584, 697, 741, 821, 934, 1021, 1160, 1215, 1312, 1417, 1462, 1591, 1693, 1729, 1845, 1910, 1982, 2127, 2170, 2339, 2365, 2499, 2529, 2639, 2745, 2864, 2950, 2992, 3119, 3235, 3255, 3559, 3620, 3754, 3835, 3943, 3975, 4061, 4210, 4270, 4371, 4417, 4502, 4640, 4677, 4822, 4904, 5026, 5113, 5173, 5271, 5317, 5426, 5492, 5583, 5740, 5757, 5839, 5935, 6033, 6146, 6212, 6369, 6454, 6557, 6597, 6711, 6983, 7047, 7173, 7202, 7310, 7421, 7451, 7579, 7666, 7785, 7831, 7981, 8060, 8128, 8251, 8326, 8369, 8445, 8569, 8638, 8761, 8873, 8923, 9017, 9104, 9239, 9283, 9368, 9500, 9586, 9683, 9782, 9794, 9908, 9989, 10123, 10327, 10442, 10535, 10658, 10739, 10803, 10925, 11006, 11060, 11198, 11225, 11326, 11474, 11554, 11663, 11723, 11810, 11902, 11987, 12027, 12117, 12261, 12320, 12419, 12532, 12646, 12676, 12808, 12915, 12941, 13067, 13113, 13246, 13360, 13426, 13520, 13811, 13862, 13936, 14073, 14102, 14206, 14305, 14408, 14527, 14555, 14650, 14755, 14816, 14951, 15031, 15107, 15226, 15326, 15392, 15484, 15553, 15623, 15734, 15872, 15943, 16043, 16087, 16201, 16299, 16355, 16444, 16514, 16635, 16723, 16802, 16912, 17150, 17285, 17387, 17488, 17533, 17603, 17708, 17793, 17932, 18026, 18081, 18159, 18285, 18356, 18395, 18532, 18644, 18697, 18761, 18874, 18937, 19107, 19119, 19251, 19379, 19414, 19522, 19619, 19691, 19748, 19875, 19935, 20065, 20109, 20261, 20315, 20559, 20703, 20737, 20876, 20950, 21069, 21106, 21231, 21323, 21379, 21494, 21611, 21680, 21796, 21805, 21958, 22027, 22091, 22167, 22324, 22347, 22459, 22551, 22691, 22761, 22822, 22951, 22981, 23089, 23216, 23290, 23402, 23453, 23529, 23668, 23743, 24019, 24057, 24214, 24249, 24335, 24445, 24554, 24619, 24704, 24761, 24847, 24947, 25089, 25205, 25274, 25352, 25474, 25537, 25612, 25711, 25748, 25874, 25984, 26078, 26155, 26237, 26324, 26378, 26545, 26623, 26720, 26774, 26855, 26953, 27021, 27123};
bool isReservedTone(int64_t k, int ld, int gi) {
    static const std::vector<uint8_t> mask = [] {
        std::vector<uint8_t> m(8 * kL1Carriers, 0);
        for (int v : kReservedS0) m[(size_t)v] = 1;
        return m;
    }();
    const int64_t P = 8 * kL1Carriers;
    const int64_t r = (((k - (int64_t)dx(gi) * (ld % kDy)) % P) + P) % P;
    return mask[(size_t)r] != 0;
}
bool isContinualPilot(int64_t k) {
    static const std::array<uint8_t, kL1Carriers> mask = [] {
        std::array<uint8_t, kL1Carriers> m{};
        for (int c : kContinualPilots) m[(size_t)c] = 1;
        return m;
    }();
    return mask[(size_t)floorMod(k, kL1Carriers)] != 0;
}

// ---------------------------------------------------------------- Reed-Muller (32,16) (clause 7.2.2)
namespace {
// generator rows as 32-bit masks (bit j = column j): 1, x1 .. x5, then the ten products x_a x_b (a < b)
struct RmTables {
    uint32_t g[16];
    uint8_t scr[32];   // w^RM
    RmTables() {
        uint32_t x[6];
        x[0] = 0xFFFFFFFFu;
        for (int m = 1; m <= 5; m++) { x[m] = 0; for (int j = 0; j < 32; j++) if ((j >> (5 - m)) & 1) x[m] |= 1u << j; }
        int n = 0;
        for (int m = 0; m <= 5; m++) g[n++] = x[m];
        for (int a = 1; a <= 5; a++) for (int b = a + 1; b <= 5; b++) g[n++] = x[a] & x[b];
        static const uint8_t w[32] = {1, 1, 1, 1, 1, 0, 0, 1, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 1, 1, 0, 0, 0, 0, 1, 1};
        memcpy(scr, w, 32);
    }
};
const RmTables& rm() { static RmTables t; return t; }
} // namespace

uint32_t rmEncode(uint16_t info) {
    uint32_t cw = 0;
    for (int i = 0; i < 16; i++) if ((info >> (15 - i)) & 1) cw ^= rm().g[i];   // b_0 is the MSB of the field word
    return cw;
}

uint16_t rmDecode(const float* llr, float* metric) {
    // The second-order coefficients (b6 .. b15, 1024 cases) by enumeration; for each, the first-order part by a fast Hadamard transform
    // over the 5 variables (b1 .. b5) with the sign giving b0. llr > 0 = bit 0: the codeword bit c_j contributes (1 - 2 c_j) llr_j.
    const RmTables& t = rm();
    float best = -1e30f;
    uint16_t bestInfo = 0;
    float r[32], h[32];
    for (int sec = 0; sec < 1024; sec++) {
        uint32_t cw2 = 0;
        for (int i = 0; i < 10; i++) if ((sec >> (9 - i)) & 1) cw2 ^= t.g[6 + i];
        for (int j = 0; j < 32; j++) r[j] = ((cw2 >> j) & 1) ? -llr[j] : llr[j];
        // Hadamard over j: H[m] = sum_j r[j] (-1)^{popcount(m & j)}; the first-order word (b1..b5) = m means codeword bit = parity(m & j)
        memcpy(h, r, sizeof h);
        for (int len = 1; len < 32; len <<= 1)
            for (int i = 0; i < 32; i += 2 * len)
                for (int j = i; j < i + len; j++) { const float a = h[j], b = h[j + len]; h[j] = a + b; h[j + len] = a - b; }
        for (int m = 0; m < 32; m++) {
            const float v = std::fabs(h[m]);
            if (v > best) {
                best = v;
                // m's bit (4 - (q - 1)) is b_q: x_q has column bit (5 - q) of j, i.e. x_q <-> j bit (5 - q)
                int b0 = h[m] < 0 ? 1 : 0;
                uint16_t info = (uint16_t)(b0 << 15);
                for (int q = 1; q <= 5; q++) if ((m >> (5 - q)) & 1) info |= (uint16_t)(1 << (15 - q));
                info |= (uint16_t)sec;
                bestInfo = info;
            }
        }
    }
    if (metric) *metric = best;
    return bestInfo;
}

void encodeHeader(uint16_t info, bool hem, std::vector<cf32>& cells) {
    const uint32_t cw = rmEncode(info);
    uint8_t lam[32], v[32];
    for (int i = 0; i < 32; i++) lam[i] = (cw >> i) & 1;
    for (int i = 0; i < 32; i++) v[(i + 2) % 32] = lam[i] ^ rm().scr[(i + 2) % 32];   // u_(i+2) = lambda_i, v_i = u_i xor w_i
    cells.clear();
    if (!hem) {
        for (int i = 0; i < 32; i++) cells.push_back(qamPoint(0, false, (unsigned)(lam[i] << 1 | v[i])));   // QPSK: y0 real, y1 imaginary
    } else {
        for (int i = 0; i < 16; i++) cells.push_back(qamPoint(1, false, (unsigned)(lam[2 * i] << 3 | lam[2 * i + 1] << 2 | v[2 * i] << 1 | v[2 * i + 1])));
    }
}

uint16_t decodeHeader(const cf32* cells, const float* n0, bool hem, float* metric) {
    float llr[32] = {};
    float l[4];
    PlpFec f;
    if (!hem) {
        f.mod = 0;
        for (int i = 0; i < 32; i++) {
            qamDemapBlock(f, cells + i, n0 + i, 1, l);   // l[0] = y0 (lambda_i), l[1] = y1 (v_i)
            llr[i] += l[0];
            const int src = (i + 30) % 32;              // v_i carries lambda_(i-2)
            llr[src] += rm().scr[i] ? -l[1] : l[1];
        }
    } else {
        f.mod = 1;
        for (int i = 0; i < 16; i++) {
            qamDemapBlock(f, cells + i, n0 + i, 1, l);   // y0 y1 y2 y3 = lambda_2i, lambda_2i+1, v_2i, v_2i+1
            llr[2 * i] += l[0];
            llr[2 * i + 1] += l[1];
            const int a = (2 * i + 30) % 32, b = (2 * i + 31) % 32;
            llr[a] += rm().scr[2 * i] ? -l[2] : l[2];
            llr[b] += rm().scr[2 * i + 1] ? -l[3] : l[3];
        }
    }
    // normalise so that the metric reads as a fraction of 32 agreeing bits (soft)
    float mag = 0;
    for (float x : llr) mag += std::fabs(x);
    float m = 0;
    const uint16_t info = rmDecode(llr, &m);
    if (metric) *metric = mag > 0 ? 32.f * m / mag : 0.f;
    return info;
}

float headerCorrelation(const cf32* cells, bool hem) {
    // soft bits of the two branches without a noise estimate (the scale cancels in the normalised correlation)
    float up[32], lo[32];
    const float one[1] = {1.f};
    float l[4];
    PlpFec f;
    f.mod = hem ? 1 : 0;
    for (int i = 0; i < (hem ? 16 : 32); i++) {
        qamDemapBlock(f, cells + i, one, 1, l);
        if (!hem) { up[i] = l[0]; lo[(i + 30) % 32] = rm().scr[i] ? -l[1] : l[1]; }
        else {
            up[2 * i] = l[0]; up[2 * i + 1] = l[1];
            lo[(2 * i + 30) % 32] = rm().scr[2 * i] ? -l[2] : l[2];
            lo[(2 * i + 31) % 32] = rm().scr[2 * i + 1] ? -l[3] : l[3];
        }
    }
    double s = 0, a = 0, b = 0;
    for (int i = 0; i < 32; i++) { s += (double)up[i] * lo[i]; a += (double)up[i] * up[i]; b += (double)lo[i] * lo[i]; }
    return a > 0 && b > 0 ? (float)(s / std::sqrt(a * b)) : 0.f;
}

// ---------------------------------------------------------------- preamble cells (clause 9.3.4) and the time interleaver (9.4.4)
int preambleDataCarrier(int q) { return q + q / 5 + 1; }

void tiWriteOrder(int depth, int width, std::vector<std::pair<int, int>>& order) {
    order.resize((size_t)depth * (size_t)width);
    for (int i = 0; i < depth * width; i++) {
        const int c = i % width, t = c % depth, r = (t + i / width) % depth;
        order[(size_t)i] = {r, c};
    }
}

void SliceCells::dataCarriers(int ld, std::vector<int64_t>& out) const {
    out.clear();
    for (int64_t k = kMin; k <= kMax; k++) if (isData(k, ld)) out.push_back(k);
}

} // namespace dvbc2
} // namespace dect2
