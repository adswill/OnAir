#include "dect2/t2/t2fec.h"
#include "dect2/t2/t2fec_tables.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <cstdint>
#include <vector>
#include "dect2/common/bits.h"

namespace dect2 {

// ============================================================================ dimensions
namespace {
const int kNormalK[6] = {32400, 38880, 43200, 48600, 51840, 54000};
const int kNormalKbch[6] = {32208, 38688, 43040, 48408, 51648, 53840};
const int kNormalQ[6] = {90, 72, 60, 45, 36, 30};
const int kNormalT[6] = {12, 12, 10, 12, 12, 10};
const int kShortK[8] = {7200, 9720, 10800, 11880, 12600, 13320, 5400, 6480};
const int kShortKbch[8] = {7032, 9552, 10632, 11712, 12432, 13152, 5232, 6312};
const int kShortQ[8] = {25, 18, 15, 12, 10, 8, 30, 27};
const char* kRateNames[8] = {"1/2", "3/5", "2/3", "3/4", "4/5", "5/6", "1/3", "2/5"};
} // namespace

const char* rateName(int r) { return (r >= 0 && r < 8) ? kRateNames[r] : "?"; }

FecDims fecDims(const PlpFec& f) {
    FecDims d;
    if (f.mod < 0 || f.mod > 3 || f.rate < 0 || f.rate > 7) return d;
    if (!f.shortFrame) {
        if (f.rate > 5) return d;
        d.nLdpc = 64800; d.kLdpc = kNormalK[f.rate]; d.kBch = kNormalKbch[f.rate]; d.q = kNormalQ[f.rate]; d.t = kNormalT[f.rate];
    } else {
        d.nLdpc = 16200; d.kLdpc = kShortK[f.rate]; d.kBch = kShortKbch[f.rate]; d.q = kShortQ[f.rate]; d.t = 12;
    }
    d.bitsPerCell = 2 * (f.mod + 1);
    d.cellsPerBlock = d.nLdpc / d.bitsPerCell;
    d.ok = true;
    return d;
}

// ============================================================================ LDPC
const LdpcCode& ldpcFor(const PlpFec& f) {
    static std::mutex mu;
    static std::map<int, LdpcCode*> cache;
    std::lock_guard<std::mutex> lk(mu);
    int key = (f.shortFrame ? 100 : 0) + f.rate;
    auto it = cache.find(key);
    if (it != cache.end()) return *it->second;
    FecDims d = fecDims(f);
    static const char* nn[6] = {"1_2", "3_5", "2_3", "3_4", "4_5", "5_6"};
    std::string name = f.rate < 6 ? std::string(nn[f.rate]) : (f.rate == 6 ? "1_3" : "2_5");
    name += f.shortFrame ? "S" : "N";
    const LdpcTable* tab = nullptr;
    for (int i = 0; i < kNumLdpcTables; i++) if (name == kLdpcTables[i].name) tab = &kLdpcTables[i];
    std::vector<std::vector<int>> rows;
    if (tab) for (int r = 0; r < tab->rows; r++) {
        const int* row = tab->data + (size_t)r * tab->cols;
        rows.emplace_back(row + 1, row + 1 + row[0]);
    }
    auto* c = new LdpcCode(d.kLdpc, d.nLdpc, rows);
    cache[key] = c;
    return *c;
}

// ============================================================================ BCH: core/src/fec/fec_proto.cpp (C++) or the Rust crate onair_fec (BchCode wraps fecp::Bch)
BchCode::BchCode(bool shortFrame, int t) : bch_(shortFrame, t) {}
void BchCode::encode(std::vector<uint8_t>& bits, int k) const { bch_.encode(bits, k); }
int BchCode::decode(std::vector<uint8_t>& bits) const { return bch_.decode(bits); }

const BchCode& bchFor(const PlpFec& f) {
    static std::mutex mu;
    static std::map<int, BchCode*> cache;
    std::lock_guard<std::mutex> lk(mu);
    FecDims d = fecDims(f);
    int key = (f.shortFrame ? 100 : 0) + d.t;
    auto it = cache.find(key);
    if (it != cache.end()) return *it->second;
    auto* c = new BchCode(f.shortFrame, d.t);
    cache[key] = c;
    return *c;
}

// ============================================================================ bit interleaver
const std::vector<int>& bitInterleaverMap(const PlpFec& f) {
    static std::mutex mu;
    static std::map<int, std::vector<int>*> cache;
    std::lock_guard<std::mutex> lk(mu);
    int key = (f.shortFrame ? 1000 : 0) + f.rate * 10 + f.mod;
    auto it = cache.find(key);
    if (it != cache.end()) return *it->second;
    FecDims d = fecDims(f);
    const int N = d.nLdpc, nbch = d.kLdpc, q = d.q, M = d.bitsPerCell;
    auto* mapp = new std::vector<int>(N);
    std::vector<int>& out = *mapp;
    // parity interleaver
    std::vector<int> tu(N);
    for (int k = 0; k < nbch; k++) tu[k] = k;
    for (int t = 0; t < q; t++) for (int s = 0; s < 360; s++) tu[nbch + 360 * t + s] = nbch + q * s + t;
    if (f.mod == 0) {
        if (f.shortFrame && (f.rate == 6 || f.rate == 7)) out = tu; // QPSK: only 1/3 and 2/5 are parity-interleaved
        else for (int i = 0; i < N; i++) out[i] = i;
    } else {
        const int* twist; const int* mux; int cols, rowsDiv;
        const bool sh = f.shortFrame;
        if (f.mod == 1) { twist = sh ? k_twist16s : k_twist16n; cols = 8;
            mux = (!sh && f.rate == 1) ? k_mux16_35 : (sh && f.rate == 6) ? k_mux16_13 : (sh && f.rate == 7) ? k_mux16_25 : k_mux16; }
        else if (f.mod == 2) { twist = sh ? k_twist64s : k_twist64n; cols = 12;
            mux = (!sh && f.rate == 1) ? k_mux64_35 : (sh && f.rate == 6) ? k_mux64_13 : (sh && f.rate == 7) ? k_mux64_25 : k_mux64; }
        else if (!sh) { twist = k_twist256n; cols = 16;
            mux = f.rate == 1 ? k_mux256_35 : f.rate == 2 ? k_mux256_23 : k_mux256; }
        else { twist = k_twist256s; cols = 8;
            mux = f.rate == 6 ? k_mux256s_13 : f.rate == 7 ? k_mux256s_25 : k_mux256s; }
        rowsDiv = cols;
        const int rows = N / rowsDiv;
        std::vector<int> tv(N);
        int idx = 0;
        for (int col = 0; col < cols; col++) {
            int offset = twist[col];
            for (int row = 0; row < rows; row++) { tv[offset + rows * col] = tu[idx++]; offset = (offset + 1) % rows; }
        }
        idx = 0;
        for (int j = 0; j < rows; j++) for (int c = 0; c < cols; c++) tu[idx++] = tv[rows * c + j];
        // cells: groups of `cols` bits; label bit order follows the mux table
        int outp = 0;
        for (int g = 0; g < N / cols; g++) {
            for (int e = 0; e < cols; e++) out[outp++] = tu[g * cols + mux[e]];
        }
    }
    cache[key] = mapp;
    (void)M;
    return *mapp;
}

// ============================================================================ constellations
namespace {
struct QamTables {
    std::vector<cf32> plain[4], rot[4];
    QamTables() {
        auto build = [&](int mod, int n, const int8_t (*tab)[2], double norm, double angDeg) {
            plain[mod].resize(n); rot[mod].resize(n);
            double a = angDeg * M_PI / 180.0, c = std::cos(a), s = std::sin(a);
            for (int i = 0; i < n; i++) {
                double re = tab[i][0] / norm, im = tab[i][1] / norm;
                plain[mod][i] = cf32((float)re, (float)im);
                rot[mod][i] = cf32((float)(re * c - im * s), (float)(re * s + im * c));
            }
        };
        build(0, 4, kConstQpsk, std::sqrt(2.0), 29.0);
        build(1, 16, kConstQam16, std::sqrt(10.0), 16.8);
        build(2, 64, kConstQam64, std::sqrt(42.0), 8.6);
        build(3, 256, kConstQam256, std::sqrt(170.0), 3.576334375);
    }
};
const QamTables& qam() { static QamTables t; return t; }
} // namespace

cf32 qamPoint(int mod, bool rotated, unsigned label) { return (rotated ? qam().rot[mod] : qam().plain[mod])[label]; }

void qamMapBlock(const PlpFec& f, const std::vector<uint16_t>& labels, std::vector<cf32>& cells) {
    const int n = (int)labels.size();
    cells.resize(n);
    const auto& tab = f.rotation ? qam().rot[f.mod] : qam().plain[f.mod];
    if (!f.rotation) { for (int j = 0; j < n; j++) cells[j] = tab[labels[j]]; return; }
    for (int j = 0; j < n; j++) cells[j] = cf32(tab[labels[j]].real(), tab[labels[(j + n - 1) % n]].imag());
}

// Exact max-log demapper over the full constellation (reference for tests).
void qamDemapBlockExact(const PlpFec& f, const cf32* cells, const float* n0, int nCells, float* llr) {
    const int M = 2 * (f.mod + 1), P = 1 << M;
    const auto& tab = f.rotation ? qam().rot[f.mod] : qam().plain[f.mod];
    std::vector<float> d(P);
    for (int s = 0; s < nCells; s++) {
        float yI, yQ, nI, nQ;
        if (f.rotation) { yI = cells[s].real(); nI = n0[s]; int s1 = (s + 1) % nCells; yQ = cells[s1].imag(); nQ = n0[s1]; }
        else { yI = cells[s].real(); yQ = cells[s].imag(); nI = nQ = n0[s]; }
        const float wI = 1.f / std::max(1e-9f, 2 * nI), wQ = 1.f / std::max(1e-9f, 2 * nQ);
        for (int p = 0; p < P; p++) {
            float dr = yI - tab[p].real(), di = yQ - tab[p].imag();
            d[p] = dr * dr * wI + di * di * wQ;
        }
        for (int b = 0; b < M; b++) {
            float m0 = 1e30f, m1 = 1e30f;
            const int sh = M - 1 - b;
            for (int p = 0; p < P; p++) { float v = d[p]; if ((p >> sh) & 1) { if (v < m1) m1 = v; } else if (v < m0) m0 = v; }
            llr[(size_t)s * M + b] = m1 - m0;
        }
    }
}

namespace {
// The square QAM labels are separable: some label bits select the in-phase level, the others the quadrature level.
struct AxisMap {
    int bitsPerAxis = 0;
    std::vector<int> reBits, imBits;      // label bit positions (0 = MSB), most significant axis bit first
    std::vector<float> reLevel, imLevel;  // coordinate for each axis index
    cf32 derot;                           // e^{-j phi}
};
const AxisMap& axisMap(int mod) {
    static AxisMap maps[4];
    static std::once_flag once;
    std::call_once(once, [] {
        for (int mod = 0; mod < 4; mod++) {
            AxisMap& A = maps[mod];
            const int M = 2 * (mod + 1), P = 1 << M;
            const auto& pl = qam().plain[mod];
            for (int b = 0; b < M; b++) {
                const int sh = M - 1 - b;
                bool movesRe = false, movesIm = false;
                for (int p = 0; p < P; p++) {
                    int q = p ^ (1 << sh);
                    if (std::fabs(pl[p].real() - pl[q].real()) > 1e-6f) movesRe = true;
                    if (std::fabs(pl[p].imag() - pl[q].imag()) > 1e-6f) movesIm = true;
                }
                (movesRe ? A.reBits : A.imBits).push_back(b);
            }
            A.bitsPerAxis = M / 2;
            const int L = 1 << A.bitsPerAxis;
            A.reLevel.assign(L, 0); A.imLevel.assign(L, 0);
            for (int p = 0; p < P; p++) {
                int ri = 0, ii = 0;
                for (int b : A.reBits) ri = (ri << 1) | ((p >> (M - 1 - b)) & 1);
                for (int b : A.imBits) ii = (ii << 1) | ((p >> (M - 1 - b)) & 1);
                A.reLevel[ri] = pl[p].real();
                A.imLevel[ii] = pl[p].imag();
            }
        }
    });
    return maps[mod];
}
const float kRotDeg[4] = {29.f, 16.8f, 8.6f, 3.576334375f};

} // namespace

namespace {
// Max-log LLRs of a block, eight cells at a time: per axis the squared distance to each of the 2^NB levels, and per axis bit the smallest
// distance among the levels with that bit 1 minus the smallest among those with it 0. Exact (the table with linear interpolation it replaced
// was within about 1e-3), and the loops over the eight lanes run as vector instructions (about five times faster than the table).
// With `map` the values go straight to their places in the LDPC codeword (the bit de-interleaver), else in label-bit order.
template <int NB>
void demapAxes(const PlpFec& f, const cf32* cells, const float* n0, int nCells, float* llr, const int* map) {
    constexpr int L = 1 << NB, M = 2 * NB, B = 8;
    const AxisMap& A = axisMap(f.mod);
    float levR[L], levI[L];
    for (int i = 0; i < L; i++) { levR[i] = A.reLevel[(size_t)i]; levI[i] = A.imLevel[(size_t)i]; }
    int posR[NB], posI[NB];
    for (int b = 0; b < NB; b++) { posR[b] = A.reBits[(size_t)b]; posI[b] = A.imBits[(size_t)b]; }
    float cr = 1.f, sr = 0.f;
    if (f.rotation) { const double a = kRotDeg[f.mod] * M_PI / 180.0; cr = (float)std::cos(a); sr = (float)std::sin(a); }
    for (int s0 = 0; s0 < nCells; s0 += B) {
        const int cnt = std::min(B, nCells - s0);
        float yI[B], yQ[B], w[B];
        for (int l = 0; l < B; l++) {
            const int s = s0 + (l < cnt ? l : 0);
            float nv;
            if (f.rotation) {
                const int s1 = s + 1 < nCells ? s + 1 : 0;
                // derotate the (I from this cell, Q from the next cell) observation back onto the unrotated grid
                const float aI = cells[s].real(), aQ = cells[s1].imag();
                yI[l] = aI * cr + aQ * sr;
                yQ[l] = aQ * cr - aI * sr;
                nv = 0.5f * (n0[s] + n0[s1]);
            } else { yI[l] = cells[s].real(); yQ[l] = cells[s].imag(); nv = n0[s]; }
            w[l] = 1.f / std::max(1e-9f, 2 * nv);
        }
        float outR[NB][B], outI[NB][B];
        for (int axis = 0; axis < 2; axis++) {
            const float* y = axis ? yQ : yI;
            const float* lev = axis ? levI : levR;
            float m0[NB][B], m1[NB][B];
            for (int b = 0; b < NB; b++) for (int l = 0; l < B; l++) { m0[b][l] = 1e30f; m1[b][l] = 1e30f; }
            for (int i = 0; i < L; i++) {
                float e[B];
                for (int l = 0; l < B; l++) { const float d = y[l] - lev[i]; e[l] = d * d; }
                for (int b = 0; b < NB; b++) {
                    if ((i >> (NB - 1 - b)) & 1) { for (int l = 0; l < B; l++) m1[b][l] = std::min(m1[b][l], e[l]); }
                    else { for (int l = 0; l < B; l++) m0[b][l] = std::min(m0[b][l], e[l]); }
                }
            }
            float (*out)[B] = axis ? outI : outR;
            for (int b = 0; b < NB; b++) for (int l = 0; l < B; l++) out[b][l] = w[l] * (m1[b][l] - m0[b][l]);
        }
        for (int l = 0; l < cnt; l++) {
            const size_t o = (size_t)(s0 + l) * M;
            if (map) for (int b = 0; b < NB; b++) { llr[map[o + (size_t)posR[b]]] = outR[b][l]; llr[map[o + (size_t)posI[b]]] = outI[b][l]; }
            else for (int b = 0; b < NB; b++) { llr[o + (size_t)posR[b]] = outR[b][l]; llr[o + (size_t)posI[b]] = outI[b][l]; }
        }
    }
}
void demapDispatch(const PlpFec& f, const cf32* cells, const float* n0, int nCells, float* llr, const int* map) {
    switch (f.mod) {
    case 0: demapAxes<1>(f, cells, n0, nCells, llr, map); break;
    case 1: demapAxes<2>(f, cells, n0, nCells, llr, map); break;
    case 2: demapAxes<3>(f, cells, n0, nCells, llr, map); break;
    default: demapAxes<4>(f, cells, n0, nCells, llr, map); break;
    }
}
} // namespace

void qamDemapBlock(const PlpFec& f, const cf32* cells, const float* n0, int nCells, float* llr) { demapDispatch(f, cells, n0, nCells, llr, nullptr); }

void qamDemapBlockToCodeword(const PlpFec& f, const cf32* cells, const float* n0, int nCells, float* llr) {
    demapDispatch(f, cells, n0, nCells, llr, bitInterleaverMap(f).data());
}

// ============================================================================ cell / time interleaver
namespace {
struct CiParams { int cell, degree, xorSize; int logic[6]; };
CiParams ciParams(const PlpFec& f) {
    CiParams p{};
    static const int l11[2] = {0, 3}, l12[2] = {0, 2}, l13[4] = {0, 1, 4, 6}, l14[6] = {0, 1, 4, 5, 9, 11}, l15[4] = {0, 1, 2, 12};
    auto set = [&](int cell, int deg, int x, const int* l) { p.cell = cell; p.degree = deg; p.xorSize = x; for (int i = 0; i < x; i++) p.logic[i] = l[i]; };
    if (!f.shortFrame) {
        switch (f.mod) { case 0: set(32400, 15, 4, l15); break; case 1: set(16200, 14, 6, l14); break; case 2: set(10800, 14, 6, l14); break; default: set(8100, 13, 4, l13); }
    } else {
        switch (f.mod) { case 0: set(8100, 13, 4, l13); break; case 1: set(4050, 12, 2, l12); break; case 2: set(2700, 12, 2, l12); break; default: set(2025, 11, 2, l11); }
    }
    return p;
}

const std::vector<int>& ciPermutation(const PlpFec& f) {
    static std::mutex mu;
    static std::map<int, std::vector<int>*> cache;
    std::lock_guard<std::mutex> lk(mu);
    int key = (f.shortFrame ? 10 : 0) + f.mod;
    auto it = cache.find(key);
    if (it != cache.end()) return *it->second;
    CiParams p = ciParams(f);
    auto* perm = new std::vector<int>();
    const int maxStates = 1 << p.degree;
    const int mask = (1 << (p.degree - 1)) - 1; // reference: 0x3fff for degree 15, 0x1fff for 14 ... i.e. degree-1 bits
    int lfsr = 0;
    for (int i = 0; i < maxStates; i++) {
        if (i == 0 || i == 1) lfsr = 0;
        else if (i == 2) lfsr = 1;
        else {
            int result = 0;
            for (int k = 0; k < p.xorSize; k++) result ^= (lfsr >> p.logic[k]) & 1;
            lfsr &= mask;
            lfsr >>= 1;
            lfsr |= result << (p.degree - 2);
        }
        lfsr |= (i % 2) << (p.degree - 1);
        if (lfsr < p.cell) perm->push_back(lfsr);
    }
    cache[key] = perm;
    return *perm;
}

// per-FEC-block cyclic shifts for one time-interleaver block
std::vector<int> ciShifts(const CiParams& p, int nBlocks) {
    std::vector<int> sh(nBlocks);
    int n = 0;
    for (int r = 0; r < nBlocks; r++) {
        int shift = p.cell;
        while (shift >= p.cell) {
            int temp = n;
            shift = 0;
            for (int q = 0; q < p.degree; q++) { shift |= temp & 1; shift <<= 1; temp >>= 1; }
            n++;
        }
        sh[r] = shift;
    }
    return sh;
}

struct TiLayout { int small, big, numBig, numSmall; };
TiLayout tiLayout(int fecBlocks, int tiBlocks) {
    TiLayout t;
    if (tiBlocks == 0) { t.small = t.big = 1; t.numBig = 0; t.numSmall = fecBlocks; }
    else {
        t.small = fecBlocks / tiBlocks;
        t.big = (fecBlocks + tiBlocks - 1) / tiBlocks;
        t.numBig = fecBlocks % tiBlocks;
        t.numSmall = tiBlocks - t.numBig;
    }
    return t;
}

template <class T>
void ciForward(const PlpFec& f, int blocks, int tiBlocks, const T* in, T* out) {
    CiParams p = ciParams(f);
    const auto& perm = ciPermutation(f);
    TiLayout tl = tiLayout(blocks, tiBlocks);
    std::vector<T> ti((size_t)blocks * p.cell);
    int index = 0;
    for (int s = 0; s < tl.numSmall + tl.numBig; s++) {
        int nb = s < tl.numSmall ? tl.small : tl.big;
        auto sh = ciShifts(p, nb);
        for (int r = 0; r < nb; r++) {
            for (int w = 0; w < p.cell; w++) ti[((perm[w] + sh[r]) % p.cell) + index] = *in++;
            index += p.cell;
        }
    }
    if (tiBlocks != 0) {
        int tiIndex = 0;
        for (int s = 0; s < tl.numSmall + tl.numBig; s++) {
            int nb = s < tl.numSmall ? tl.small : tl.big;
            int numCols = 5 * nb, rows = p.cell / 5;
            for (int k = 0; k < rows; k++) for (int w = 0; w < numCols; w++) *out++ = ti[tiIndex + rows * w + k];
            tiIndex += rows * numCols;
        }
    } else {
        for (size_t i = 0; i < ti.size(); i++) *out++ = ti[i];
    }
}

template <class T>
void ciInverse(const PlpFec& f, int blocks, int tiBlocks, const T* in, T* out) {
    CiParams p = ciParams(f);
    const auto& perm = ciPermutation(f);
    TiLayout tl = tiLayout(blocks, tiBlocks);
    std::vector<T> ti((size_t)blocks * p.cell);
    if (tiBlocks != 0) {
        int tiIndex = 0;
        for (int s = 0; s < tl.numSmall + tl.numBig; s++) {
            int nb = s < tl.numSmall ? tl.small : tl.big;
            int numCols = 5 * nb, rows = p.cell / 5;
            for (int k = 0; k < rows; k++) for (int w = 0; w < numCols; w++) ti[tiIndex + rows * w + k] = *in++;
            tiIndex += rows * numCols;
        }
    } else {
        for (size_t i = 0; i < ti.size(); i++) ti[i] = *in++;
    }
    int index = 0;
    for (int s = 0; s < tl.numSmall + tl.numBig; s++) {
        int nb = s < tl.numSmall ? tl.small : tl.big;
        auto sh = ciShifts(p, nb);
        for (int r = 0; r < nb; r++) {
            for (int w = 0; w < p.cell; w++) *out++ = ti[((perm[w] + sh[r]) % p.cell) + index];
            index += p.cell;
        }
    }
}
} // namespace

void cellInterleave(const PlpFec& f, int blocks, int tiBlocks, const std::vector<cf32>& in, std::vector<cf32>& out) {
    out.resize(in.size());
    ciForward(f, blocks, tiBlocks, in.data(), out.data());
}
void cellDeinterleave(const PlpFec& f, int blocks, int tiBlocks, const cf32* in, cf32* out) { ciInverse(f, blocks, tiBlocks, in, out); }
void cellDeinterleaveF(const PlpFec& f, int blocks, int tiBlocks, const float* in, float* out) { ciInverse(f, blocks, tiBlocks, in, out); }

// ============================================================================ baseband framing
const uint8_t* bbRandomiser() {
    static std::vector<uint8_t> r = [] {
        std::vector<uint8_t> v(64800);
        int sr = 0x4A80;
        for (int i = 0; i < 64800; i++) {
            int b = (sr ^ (sr >> 1)) & 1;
            v[i] = b;
            sr >>= 1;
            if (b) sr |= 0x4000;
        }
        return v;
    }();
    return r.data();
}

uint8_t crc8Bits(const uint8_t* bits, int n) {
    // CRC-8, polynomial x^8+x^7+x^6+x^4+x^2+1 (0xD5), MSB first, zero initial value
    unsigned crc = 0;
    for (int i = 0; i < n; i++) {
        unsigned fb = ((crc >> 7) & 1) ^ bits[i];
        crc = (crc << 1) & 0xff;
        if (fb) crc ^= 0xD5;
    }
    return (uint8_t)crc;
}

static unsigned getBits(const uint8_t* b, int pos, int n) { return (unsigned)bits::getMsbRaw(b, (size_t)pos, n); }

bool parseBbHeader(const uint8_t* b, BbHeader& h) {
    uint8_t crc = crc8Bits(b, 72);
    unsigned got = getBits(b, 72, 8);
    h.hem = crc == (got ^ 0x01u); // MODE bit is XORed into the CRC LSB
    h.crcOk = crc == got || h.hem;
    h.tsGs = getBits(b, 0, 2); h.sisMis = b[2]; h.ccmAcm = b[3]; h.issyi = b[4]; h.npd = b[5]; h.ro = getBits(b, 6, 2);
    h.isi = getBits(b, 8, 8);
    h.upl = getBits(b, 16, 16); h.dfl = getBits(b, 32, 16); h.sync = getBits(b, 48, 8); h.syncd = getBits(b, 56, 16);
    if (h.hem) { h.issy = (getBits(b, 16, 16) << 8) | getBits(b, 48, 8); h.upl = 1504; h.sync = 0x47; }
    return h.crcOk;
}

void buildBbHeader(const BbHeader& h, uint8_t* b) {
    auto put = [&](int pos, unsigned v, int n) { for (int i = 0; i < n; i++) b[pos + i] = (v >> (n - 1 - i)) & 1; };
    put(0, h.tsGs, 2); b[2] = h.sisMis; b[3] = h.ccmAcm; b[4] = h.issyi; b[5] = h.npd; put(6, h.ro, 2);
    put(8, h.isi, 8); put(16, h.upl, 16); put(32, h.dfl, 16); put(48, h.sync, 8); put(56, h.syncd, 16);
    put(72, crc8Bits(b, 72), 8);
}

} // namespace dect2
