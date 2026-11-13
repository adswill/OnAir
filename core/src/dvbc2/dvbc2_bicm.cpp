// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DVB-C2 bit-interleaved coding and modulation of the PLPs (EN 302 769 clause 6): the FEC dimensions (tables 3(a) / 3(b)), the LDPC codes
// (DVB-T2's tables for 2/3 .. 5/6, DVB-S2's for 8/9 and 9/10: the annexes A and B of EN 302 769 list the same accumulator addresses), the
// BCH codes, the bit interleaver with the demultiplexer (tables 6, 7 and 10) and the QAM mapping (DVB-T2's unrotated constellations for
// 16 / 64 / 256-QAM; 1024-QAM and 4096-QAM, which DVB-T2 does not have, are built here from the rule of tables 12(j) - 12(m)).
#include "dect2/dvbc2/dvbc2.h"
#include "dect2/t2/t2fec_tables.h"
#include "dect2/dsp/ldpc.h"
#include "../dvbs/dvbs_ldpc_tables.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>

namespace dect2 {
namespace dvbc2 {

namespace {
// kLdpc, kBch, t per PLP_COD 1 .. 5
const int kNormalK[6] = {0, 43200, 48600, 51840, 54000, 58320};
const int kNormalKbch[6] = {0, 43040, 48408, 51648, 53840, 58192};
const int kNormalT[6] = {0, 10, 12, 12, 10, 8};
const int kShortK[6] = {0, 10800, 11880, 12600, 13320, 14400};
const int kShortKbch[6] = {0, 10632, 11712, 12432, 13152, 14232};
const int kXfecShort[8] = {900, 4050, 2700, 2025, 1620, 1350, 1158, 1013};
const int kXfecNormal[8] = {900, 16200, 10800, 8100, 6480, 5400, 4629, 4050};

// 1024-QAM and 4096-QAM bit interleaver (tables 6, 7(a), 7(b)) and demultiplexer (tables 10(a) / 10(b), the same for every code rate).
// The demultiplexer tables are stored as DVB-T2's are: entry e is the input bit number d_i mod N_substreams that becomes output bit e.
const int kTwist1024n[20] = {0, 1, 3, 4, 5, 6, 6, 9, 13, 14, 14, 16, 21, 21, 23, 25, 25, 26, 28, 30};
const int kTwist1024s[20] = {0, 0, 0, 2, 2, 2, 2, 2, 5, 5, 5, 5, 5, 7, 7, 7, 7, 8, 8, 10};
const int kTwist4096n[12] = {0, 0, 2, 2, 3, 4, 4, 5, 5, 7, 8, 9};
const int kTwist4096s[24] = {0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 7, 9, 9, 9, 10, 10, 10, 10, 10, 11};
// table 10, d_i mod N_substreams -> e
const int kDemux1024n[20] = {8, 16, 7, 19, 4, 15, 3, 12, 0, 11, 10, 9, 13, 2, 14, 5, 17, 6, 18, 1};
const int kDemux1024s[20] = {8, 3, 7, 10, 19, 4, 9, 5, 17, 6, 14, 11, 2, 18, 16, 15, 0, 1, 13, 12};
const int kDemux4096n[12] = {8, 0, 6, 1, 4, 5, 2, 3, 7, 10, 11, 9};
const int kDemux4096s[24] = {10, 15, 4, 19, 21, 16, 23, 18, 11, 14, 22, 5, 6, 17, 13, 20, 1, 3, 9, 2, 7, 8, 12, 0};
template <int N> std::vector<int> invert(const int (&t)[N]) { std::vector<int> v(N); for (int i = 0; i < N; i++) v[(size_t)t[i]] = i; return v; }

int bitsPerAxis(int mod) { return mod + 1; }
// gray code inverse
unsigned grayInverse(unsigned g) { unsigned b = 0; for (; g; g >>= 1) b ^= g; return b; }
} // namespace

// Tables 12(h) - 12(m): the real part takes the even label bits y0, y2, ..., the imaginary part the odd ones; on each axis the first bit is
// the sign (1 = negative) and the others are the reflected Gray code of the distance from the outermost point, so that with n bits per axis
// (M = 2^n levels) the level is +-(M - 1 - 2 ginv(rest)). This gives DVB-T2's 16 / 64 / 256-QAM tables exactly (the tests compare them).
float axisLevel(int n, unsigned bits) {
    const unsigned M = 1u << n;
    const unsigned sign = bits >> (n - 1) & 1u, rest = bits & ((1u << (n - 1)) - 1u);
    const float mag = (float)(M - 1u - 2u * grayInverse(rest));
    return sign ? -mag : mag;
}
cf32 cellPoint(int mod, unsigned label) {
    if (mod >= 0 && mod <= 3) return qamPoint(mod, false, label);
    const int n = bitsPerAxis(mod), M = 2 * n;
    unsigned re = 0, im = 0;
    for (int b = 0; b < M; b++) { const unsigned bit = label >> (M - 1 - b) & 1u; if (b & 1) im = im << 1 | bit; else re = re << 1 | bit; }
    const float norm = 1.f / std::sqrt(mod == 4 ? 682.f : 2730.f);   // table 13
    return cf32(axisLevel(n, re) * norm, axisLevel(n, im) * norm);
}

namespace {
// max-log LLRs (label order, positive = bit 0) of the 1024 / 4096-QAM cells, per axis over its 32 / 64 levels
void demapLarge(int mod, const cf32* cells, const float* n0, int nCells, float* llr) {
    const int n = bitsPerAxis(mod), L = 1 << n, M = 2 * n;
    const float norm = 1.f / std::sqrt(mod == 4 ? 682.f : 2730.f);
    std::vector<float> lev((size_t)L);
    for (int i = 0; i < L; i++) lev[(size_t)i] = axisLevel(n, (unsigned)i) * norm;
    float d[64];
    for (int c = 0; c < nCells; c++) {
        const float w = 1.f / std::max(1e-9f, 2 * n0[c]);
        for (int axis = 0; axis < 2; axis++) {
            const float y = axis ? cells[c].imag() : cells[c].real();
            for (int i = 0; i < L; i++) { const float e = y - lev[(size_t)i]; d[i] = e * e; }
            for (int b = 0; b < n; b++) {
                float m0 = 1e30f, m1 = 1e30f;
                const int sh = n - 1 - b;
                for (int i = 0; i < L; i++) { if (i >> sh & 1) m1 = std::min(m1, d[i]); else m0 = std::min(m0, d[i]); }
                llr[(size_t)c * M + (size_t)(2 * b + axis)] = w * (m1 - m0);
            }
        }
    }
}
} // namespace

Dims dims(const Fec& f) {
    Dims d;
    if (f.mod < 1 || f.mod > 5 || f.cod < 1 || f.cod > 5) return d;
    if (f.fecType) { d.nLdpc = 64800; d.kLdpc = kNormalK[f.cod]; d.kBch = kNormalKbch[f.cod]; d.t = kNormalT[f.cod]; }
    else { d.nLdpc = 16200; d.kLdpc = kShortK[f.cod]; d.kBch = kShortKbch[f.cod]; d.t = 12; }
    d.q = (d.nLdpc - d.kLdpc) / 360;
    d.bitsPerCell = 2 * (f.mod + 1);
    d.cells = d.nLdpc / d.bitsPerCell;
    d.ok = true;
    return d;
}

bool modCodValid(const Fec& f) {
    // tables 11(a) and 11(b) (the same for both FEC frame lengths; the 16K-only rate 1/2 cannot be signalled in PLP_COD)
    if (f.mod < 1 || f.mod > 5) return false;
    switch (f.cod) {
    case 1: return f.mod == 2;                                 // 2/3: 64QAM
    case 2: return f.mod == 3 || f.mod == 4;                   // 3/4: 256QAM, 1024QAM
    case 3: return f.mod == 1 || f.mod == 2 || f.mod == 5;     // 4/5: 16QAM, 64QAM, 4096QAM (new in V1.3.1)
    case 4: return f.mod >= 3;                                 // 5/6: 256QAM, 1024QAM, 4096QAM
    case 5: return true;                                       // 8/9 and 9/10: all
    default: return false;
    }
}

const char* modName(int mod) {
    static const char* const n[] = {"reserved", "16-QAM", "64-QAM", "256-QAM", "1024-QAM", "4096-QAM", "reserved", "reserved"};
    return mod >= 0 && mod < 8 ? n[mod] : "?";
}
const char* codName(int fecType, int cod) {
    static const char* const n[] = {"reserved", "2/3", "3/4", "4/5", "5/6", "9/10", "reserved", "reserved"};
    if (cod == 5 && !fecType) return "8/9";
    return cod >= 0 && cod < 8 ? n[cod] : "?";
}
int xfecFrameCells(int fecType, int mod) {
    if (mod < 0 || mod > 7) return 0;
    return fecType ? kXfecNormal[mod] : kXfecShort[mod];
}

namespace {
const LdpcCode& ldpcCode(const Fec& f) {
    static std::mutex mu;
    static std::map<int, std::unique_ptr<LdpcCode>> cache;
    if (f.cod >= 1 && f.cod <= 4) {   // DVB-T2's tables: PLP_COD 2 .. 5 there
        PlpFec t; t.shortFrame = !f.fecType; t.rate = f.cod + 1; t.mod = f.mod;
        return ldpcFor(t);
    }
    std::lock_guard<std::mutex> lk(mu);
    const int key = f.fecType;
    auto it = cache.find(key);
    if (it != cache.end()) return *it->second;
    const Dims d = dims(f);
    const dvbs::S2LdpcTable& tab = dvbs::kS2LdpcTables[f.fecType ? 0 : 1][f.fecType ? 10 : 9];   // 9/10 normal, 8/9 short
    std::vector<std::vector<int>> rows;
    const uint16_t* p = tab.data;
    for (int r = 0; r < tab.rows; r++) {
        const int n = *p++;
        rows.emplace_back(p, p + n);
        p += n;
    }
    auto code = std::make_unique<LdpcCode>(d.kLdpc, d.nLdpc, rows);
    const LdpcCode& ref = *code;
    cache[key] = std::move(code);
    return ref;
}
const BchCode& bchCode(const Fec& f) {
    static std::mutex mu;
    static std::map<int, std::unique_ptr<BchCode>> cache;
    const Dims d = dims(f);
    const int key = (f.fecType ? 100 : 0) + d.t;
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(key);
    if (it != cache.end()) return *it->second;
    auto c = std::make_unique<BchCode>(!f.fecType, d.t);
    const BchCode& ref = *c;
    cache[key] = std::move(c);
    return ref;
}
} // namespace

const std::vector<int>& bitMap(const Fec& f) {
    static std::mutex mu;
    static std::map<int, std::unique_ptr<std::vector<int>>> cache;
    std::lock_guard<std::mutex> lk(mu);
    const int key = f.fecType * 100 + f.cod * 10 + f.mod;
    auto it = cache.find(key);
    if (it != cache.end()) return *it->second;
    const Dims d = dims(f);
    auto mapp = std::make_unique<std::vector<int>>(d.nLdpc);
    std::vector<int>& out = *mapp;
    const int N = d.nLdpc, nbch = d.kLdpc, q = d.q;
    // parity interleaver (6.1.3)
    std::vector<int> tu(N);
    for (int k = 0; k < nbch; k++) tu[k] = k;
    for (int t = 0; t < q; t++) for (int s = 0; s < 360; s++) tu[nbch + 360 * t + s] = nbch + q * s + t;
    // column twist (tables 6 and 7) and the demultiplexer (table 10)
    const int* twist; const int* mux; int cols;
    const bool sh = !f.fecType;
    static const std::vector<int> m1024n = invert(kDemux1024n), m1024s = invert(kDemux1024s), m4096n = invert(kDemux4096n), m4096s = invert(kDemux4096s);
    if (f.mod == 4) { twist = sh ? kTwist1024s : kTwist1024n; cols = 20; mux = (sh ? m1024s : m1024n).data(); }
    else if (f.mod == 5) { twist = sh ? kTwist4096s : kTwist4096n; cols = sh ? 24 : 12; mux = (sh ? m4096s : m4096n).data(); }
    else if (f.mod == 1) { twist = sh ? k_twist16s : k_twist16n; cols = 8; mux = k_mux16; }
    else if (f.mod == 2) { twist = sh ? k_twist64s : k_twist64n; cols = 12; mux = k_mux64; }
    else if (!sh) { twist = k_twist256n; cols = 16; mux = f.cod == 1 ? k_mux256_23 : k_mux256; }
    else { twist = k_twist256s; cols = 8; mux = k_mux256s; }
    const int rows = N / cols;
    std::vector<int> tv(N);
    int idx = 0;
    for (int col = 0; col < cols; col++) {
        int offset = twist[col];
        for (int row = 0; row < rows; row++) { tv[offset + rows * col] = tu[idx++]; offset = (offset + 1) % rows; }
    }
    idx = 0;
    for (int j = 0; j < rows; j++) for (int c = 0; c < cols; c++) tu[idx++] = tv[rows * c + j];
    int outp = 0;
    for (int g = 0; g < N / cols; g++) for (int e = 0; e < cols; e++) out[outp++] = tu[g * cols + mux[e]];
    const std::vector<int>& ref = out;
    cache[key] = std::move(mapp);
    return ref;
}

void encodeBlock(const Fec& f, const std::vector<uint8_t>& bb, std::vector<cf32>& cells) {
    const Dims d = dims(f);
    cells.clear();
    if (!d.ok || (int)bb.size() < d.kBch) return;
    std::vector<uint8_t> bits(bb.begin(), bb.begin() + d.kBch);
    const uint8_t* rnd = bbRandomiser();
    for (int i = 0; i < d.kBch; i++) bits[i] ^= rnd[i];
    bchCode(f).encode(bits, d.kBch);
    ldpcCode(f).encode(bits);
    const auto& map = bitMap(f);
    std::vector<uint16_t> lab((size_t)d.cells);
    for (int c = 0; c < d.cells; c++) {
        unsigned l = 0;
        for (int k = 0; k < d.bitsPerCell; k++) l = (l << 1) | bits[(size_t)map[(size_t)c * d.bitsPerCell + k]];
        lab[(size_t)c] = (uint16_t)l;
    }
    if (f.mod >= 4) { cells.resize(lab.size()); for (size_t c = 0; c < lab.size(); c++) cells[c] = cellPoint(f.mod, lab[c]); return; }
    PlpFec t; t.mod = f.mod; t.rotation = false;
    qamMapBlock(t, lab, cells);
}

BlockResult decodeBlock(const Fec& f, const cf32* cells, const float* n0, std::vector<uint8_t>& bb, int maxIter) {
    BlockResult r;
    const Dims d = dims(f);
    bb.clear();
    if (!d.ok) return r;
    PlpFec t; t.mod = f.mod; t.rotation = false;
    std::vector<float> lab((size_t)d.nLdpc);
    if (f.mod >= 4) demapLarge(f.mod, cells, n0, d.cells, lab.data());
    else qamDemapBlock(t, cells, n0, d.cells, lab.data());   // label order
    const auto& map = bitMap(f);
    std::vector<float> llr((size_t)d.nLdpc);
    for (int p = 0; p < d.nLdpc; p++) llr[(size_t)map[(size_t)p]] = lab[(size_t)p];
    std::vector<uint8_t> hard;
    const LdpcCode& code = ldpcCode(f);
    t2LdpcDecode(code, llr, maxIter, hard, &r.ldpcIters);
    std::vector<uint8_t> word(hard.begin(), hard.begin() + d.kLdpc);
    int corr = bchCode(f).decode(word);
    if (corr < 0) {   // second chance with more iterations and another normalisation
        int it2 = 0;
        std::vector<uint8_t> h2;
        t2LdpcDecode(code, llr, 3 * maxIter, h2, &it2, 0.66f);
        std::vector<uint8_t> w2(h2.begin(), h2.begin() + d.kLdpc);
        const int c2 = bchCode(f).decode(w2);
        if (c2 >= 0) { corr = c2; word = std::move(w2); hard = std::move(h2); r.ldpcIters = it2; }
    }
    if (corr < 0) return r;
    r.ok = true;
    r.bchCorrected = corr;
    long errs = 0;
    for (int p = 0; p < d.nLdpc; p++) errs += (llr[(size_t)p] < 0) != (hard[(size_t)p] != 0);
    r.preBer = (double)errs / d.nLdpc;
    bb.assign(word.begin(), word.begin() + d.kBch);
    const uint8_t* rnd = bbRandomiser();
    for (int i = 0; i < d.kBch; i++) bb[(size_t)i] ^= rnd[i];
    return r;
}

double blockSeconds(const Fec& f, int dataCellsPerSymbol, int gi) {
    const Dims d = dims(f);
    if (!d.ok || dataCellsPerSymbol <= 0) return 0;
    return (double)d.cells / dataCellsPerSymbol * symbolSeconds(gi);
}

double netBitrate(const Fec& f, double cellsPerSecond) {
    const Dims d = dims(f);
    if (!d.ok) return 0;
    const int npk = (d.kBch - 80) / 1504;
    return cellsPerSecond / d.cells * npk * 188 * 8;
}

} // namespace dvbc2
} // namespace dect2
