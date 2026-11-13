// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cell towers, LTE: sequences, resource grid, control region, DCI and PDSCH mapping (36.211, 36.212, 36.213). See cell_lte.h.
#include "dect2/cell/cell_lte.h"
#include <algorithm>
#include <cmath>

namespace dect2::cell {

int lteNfft(int nRb) { return nRb <= 6 ? 128 : nRb <= 15 ? 256 : nRb <= 25 ? 512 : nRb <= 50 ? 1024 : 2048; }

int lteCpLen(int lsf, bool ext, int nfft) {
    if (ext) return nfft / 4;
    return (lsf % 7 == 0) ? 160 * nfft / 2048 : 144 * nfft / 2048;
}

int lteSymStart(int lsf, bool ext, int nfft) {
    const int ns = lteSymbPerSlot(ext), slot = lsf / ns, l = lsf % ns;
    int p = slot * (15 * nfft / 2);
    for (int i = 0; i < l; i++) p += lteCpLen(i, ext, nfft) + nfft;
    return p;
}

int lteBin(int k, int nRb, int nfft) {
    const int half = 6 * nRb;
    return k < half ? nfft - half + k : k - half + 1;
}

int lteBwIndexToRb(int i) { static const int t[6] = {6, 15, 25, 50, 75, 100}; return (i >= 0 && i < 6) ? t[i] : 0; }
int lteRbToBwIndex(int n) { for (int i = 0; i < 6; i++) if (lteBwIndexToRb(i) == n) return i; return -1; }

void ltePss(int nid2, cf32 d[62]) {
    static const int u[3] = {25, 29, 34};
    for (int n = 0; n < 62; n++) {
        const double a = n < 31 ? -M_PI * u[nid2] * n * (n + 1) / 63.0 : -M_PI * u[nid2] * (n + 1) * (n + 2) / 63.0;
        d[n] = cf32((float)std::cos(a), (float)std::sin(a));
    }
}

static void mseq(int t0, int t1, int t2, int t3, int8_t out[31]) {
    // x(i+5) = sum of the taps mod 2, x(0..4) = 0 0 0 0 1; out = 1 - 2x
    int x[36] = {0, 0, 0, 0, 1};
    for (int i = 0; i + 5 < 31; i++) {
        int v = 0;
        if (t0 >= 0) v ^= x[i + t0];
        if (t1 >= 0) v ^= x[i + t1];
        if (t2 >= 0) v ^= x[i + t2];
        if (t3 >= 0) v ^= x[i + t3];
        x[i + 5] = v;
    }
    for (int i = 0; i < 31; i++) out[i] = (int8_t)(1 - 2 * x[i]);
}

void lteSss(int nid1, int nid2, int sf, float d[62]) {
    int8_t s[31], c[31], z[31];
    mseq(2, 0, -1, -1, s);       // x(i+2) + x(i)
    mseq(3, 0, -1, -1, c);       // x(i+3) + x(i)
    mseq(4, 2, 1, 0, z);         // x(i+4) + x(i+2) + x(i+1) + x(i)
    const int qp = nid1 / 30, q = (nid1 + qp * (qp + 1) / 2) / 30, mp = nid1 + q * (q + 1) / 2;
    const int m0 = mp % 31, m1 = (m0 + mp / 31 + 1) % 31;
    for (int n = 0; n < 31; n++) {
        const int s0 = s[(n + m0) % 31], s1 = s[(n + m1) % 31];
        const int c0 = c[(n + nid2) % 31], c1 = c[(n + nid2 + 3) % 31];
        const int z0 = z[(n + (m0 % 8)) % 31], z1 = z[(n + (m1 % 8)) % 31];
        if (sf == 0) { d[2 * n] = (float)(s0 * c0); d[2 * n + 1] = (float)(s1 * c1 * z0); }
        else { d[2 * n] = (float)(s1 * c0); d[2 * n + 1] = (float)(s0 * c1 * z1); }
    }
}

void lteCrsSeq(int pci, int ns, int l, bool ext, cf32 r[2 * kMaxRb]) {
    const uint32_t cinit = (uint32_t)(1024u * (uint32_t)(7 * (ns + 1) + l + 1) * (uint32_t)(2 * pci + 1) + (uint32_t)(2 * pci) + (ext ? 0u : 1u));
    uint8_t c[4 * kMaxRb];
    gold(cinit, c, 4 * kMaxRb);
    const float s = (float)M_SQRT1_2;
    for (int m = 0; m < 2 * kMaxRb; m++) r[m] = cf32(s * (1 - 2 * c[2 * m]), s * (1 - 2 * c[2 * m + 1]));
}

bool lteCrsAt(const LteCellCfg& c, int sf, int lsf, int k, int p) {
    const int N = lteSymbPerSlot(c.extCp), slot = lsf / N, l = lsf % N, ns = 2 * sf + slot;
    int v;
    if (p <= 1) {
        if (l != 0 && l != N - 3) return false;
        v = (p == 0) ? (l == 0 ? 0 : 3) : (l == 0 ? 3 : 0);
    } else {
        if (l != 1) return false;
        v = (p == 2) ? 3 * (ns % 2) : 3 + 3 * (ns % 2);
    }
    return (k % 6) == (v + c.pci % 6) % 6;
}

bool lteCrsAny(const LteCellCfg& c, int sf, int lsf, int k, int ports) {
    for (int p = 0; p < ports; p++) if (lteCrsAt(c, sf, lsf, k, p)) return true;
    return false;
}

const uint8_t kCfiCode[3][32] = {
    {0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1},
    {1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0},
    {1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1}};

int ltePhichGroups(const LteCellCfg& c) {
    static const double ng[4] = {1.0 / 6, 0.5, 1, 2};
    return (int)std::ceil(ng[c.phichRes & 3] * c.nRb / 8.0 - 1e-9);
}

// REGs of control symbol l: a REG of 6 REs (with 2 reference signal REs) where reference signals are, else of 4
static std::vector<LteReg> regsOf(const LteCellCfg& c, int l) {
    std::vector<LteReg> v;
    const int nsc = 12 * c.nRb;
    const bool rs = (l == 0) || (l == 1 && c.ports == 4) || (l == 3 && c.extCp);
    if (rs) {
        for (int k0 = 0; k0 < nsc; k0 += 6) {
            LteReg r; r.l = l; r.k0 = k0;
            int n = 0;
            for (int k = k0; k < k0 + 6; k++) if (k % 3 != (c.pci % 6) % 3) r.k[n++] = k;
            v.push_back(r);
        }
    } else {
        for (int k0 = 0; k0 < nsc; k0 += 4) { LteReg r; r.l = l; r.k0 = k0; for (int i = 0; i < 4; i++) r.k[i] = k0 + i; v.push_back(r); }
    }
    return v;
}

int lteCtrlSyms(int cfi, int nRb) { return cfi + (nRb <= 10 ? 1 : 0); }

LteControl lteControl(const LteCellCfg& c, int cfiIn, int subframe) {
    (void)subframe;
    LteControl o;
    o.cfi = cfiIn;
    const int cfi = lteCtrlSyms(cfiIn, c.nRb);   // control symbols
    const int nsc = 12 * c.nRb;
    std::vector<std::vector<LteReg>> regs((size_t)cfi);
    std::vector<std::vector<int>> used((size_t)cfi);   // 0 free, 1 PCFICH, 2 PHICH
    for (int l = 0; l < cfi; l++) { regs[(size_t)l] = regsOf(c, l); used[(size_t)l].assign(regs[(size_t)l].size(), 0); }
    // PCFICH
    const int kb = 6 * (c.pci % (2 * c.nRb));
    for (int i = 0; i < 4; i++) {
        const int k = (kb + (i * c.nRb / 2) * 6) % nsc;
        for (size_t j = 0; j < regs[0].size(); j++) if (regs[0][j].k0 == k) { used[0][j] = 1; o.pcfich.push_back(regs[0][j]); }
    }
    // PHICH
    const int units = ltePhichGroups(c);
    std::vector<std::vector<int>> freeIdx((size_t)cfi);
    for (int l = 0; l < cfi; l++) for (size_t j = 0; j < regs[(size_t)l].size(); j++) if (!used[(size_t)l][j]) freeIdx[(size_t)l].push_back((int)j);
    const int n0 = (int)freeIdx[0].size();
    for (int m = 0; m < units; m++) for (int i = 0; i < 3; i++) {
        const int l = (c.phichDur && cfi >= 3) ? i : 0;
        const int nl = (int)freeIdx[(size_t)l].size();
        if (nl == 0) continue;
        const int ni = (c.pci * nl / n0 + m + i * nl / 3) % nl;
        const int j = freeIdx[(size_t)l][(size_t)ni];
        used[(size_t)l][(size_t)j] = 2;
        o.phich.push_back(regs[(size_t)l][(size_t)j]);
    }
    // PDCCH REGs in mapping order: k first, then l
    std::vector<LteReg> list;
    std::vector<size_t> pos((size_t)cfi, 0);
    for (int k = 0; k < nsc; k++) for (int l = 0; l < cfi; l++) {
        auto& R = regs[(size_t)l];
        size_t& p = pos[(size_t)l];
        if (p < R.size() && R[p].k0 == k) { if (!used[(size_t)l][p]) list.push_back(R[p]); p++; }
    }
    const int M = (int)list.size();
    o.nCce = M / 9;
    // undo the sub-block interleaver (on quadruplets) and the cyclic shift by the cell id
    const int Rr = (M + 31) / 32, Nd = 32 * Rr - M;
    std::vector<int> w;
    w.reserve((size_t)M);
    for (int kk = 0; kk < 32 * Rr; kk++) { const int y = (kk % Rr) * 32 + kSubblockPermConv[kk / Rr]; if (y >= Nd) w.push_back(y - Nd); }
    o.pdcchQuad.assign((size_t)M, LteReg());
    for (int mp = 0; mp < M; mp++) o.pdcchQuad[(size_t)w[(size_t)((mp + c.pci) % M)]] = list[(size_t)mp];
    return o;
}

static int ceilLog2(long v) { int b = 0; while ((1L << b) < v) b++; return b; }

int lteDci1ASize(int nRb) {
    const int riv = ceilLog2((long)nRb * (nRb + 1) / 2);
    int s = 15 + riv;   // larger than format 0 (14 + riv): format 0 is padded to it
    static const int amb[] = {12, 14, 16, 20, 24, 26, 32, 40, 44, 56};
    for (int a : amb) if (s == a) { s++; break; }
    return s;
}

static void gapsFor(int nRb, int& g1, int& g2) {
    g2 = 0;
    if (nRb <= 10) g1 = (nRb + 1) / 2;
    else if (nRb == 11) g1 = 4;
    else if (nRb <= 19) g1 = 8;
    else if (nRb <= 26) g1 = 12;
    else if (nRb <= 44) g1 = 18;
    else if (nRb <= 49) g1 = 27;
    else if (nRb <= 63) { g1 = 27; g2 = 9; }
    else if (nRb <= 79) { g1 = 32; g2 = 16; }
    else { g1 = 48; g2 = 16; }
}

static int nVrbGap1(int nRb) { int g1, g2; gapsFor(nRb, g1, g2); return 2 * std::min(g1, nRb - g1); }

int lteDci1CSize(int nRb) {
    const int step = nRb < 50 ? 2 : 4;
    const int np = nVrbGap1(nRb) / step;
    return (nRb >= 50 ? 1 : 0) + ceilLog2((long)np * (np + 1) / 2) + 5;
}

bool lteRiv(int riv, int n, int& start, int& len) {
    for (int L = 1; L <= n; L++) for (int s = 0; s + L <= n; s++) if (lteRivMake(s, L, n) == riv) { start = s; len = L; return true; }
    return false;
}

int lteRivMake(int s, int L, int n) { return (L - 1 <= n / 2) ? n * (L - 1) + s : n * (n - L + 1) + (n - 1 - s); }

static uint32_t take(const Bits& b, int& p, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v = (v << 1) | (p < (int)b.size() ? b[(size_t)p++] : 0); return v; }
static void put(Bits& b, uint32_t v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((v >> i) & 1); }

bool lteDciParse(const Bits& b, int nRb, int format, LteDciSi& d) {
    d = LteDciSi();
    d.format = format;
    int p = 0;
    if (format == 0) {
        if ((int)b.size() != lteDci1ASize(nRb)) return false;
        if (take(b, p, 1) != 1) return false;          // format 0 / 1A flag: must be 1A
        d.distributed = take(b, p, 1);
        const int nb = ceilLog2((long)nRb * (nRb + 1) / 2);
        const int riv = (int)take(b, p, nb);
        d.mcs = (int)take(b, p, 5);
        take(b, p, 3);                                   // HARQ process (reserved)
        const int ndi = (int)take(b, p, 1);
        d.rv = (int)take(b, p, 2);
        const int tpc = (int)take(b, p, 2);
        d.nPrb1A = (tpc & 1) ? 3 : 2;
        d.gap = (d.distributed && nRb >= 50) ? ndi : 0;
        if (!lteRiv(riv, nRb, d.rbStart, d.lCrb)) return false;
        if (d.distributed) {
            int g1, g2; gapsFor(nRb, g1, g2);
            const int nv = d.gap ? (nRb / (2 * g2)) * 2 * g2 : nVrbGap1(nRb);
            if (d.rbStart + d.lCrb > nv) return false;
        }
        if (d.mcs > 26) return false;
        d.tbs = lteTbs(d.mcs, d.nPrb1A);
        return d.tbs > 0;
    }
    if ((int)b.size() != lteDci1CSize(nRb)) return false;
    const int step = nRb < 50 ? 2 : 4;
    d.distributed = true;
    if (nRb >= 50) d.gap = (int)take(b, p, 1);
    const int np = nVrbGap1(nRb) / step;
    const int riv = (int)take(b, p, ceilLog2((long)np * (np + 1) / 2));
    d.mcs = (int)take(b, p, 5);
    int s, L;
    if (!lteRiv(riv, np, s, L)) return false;
    d.rbStart = s * step; d.lCrb = L * step;
    d.tbs = lteTbs1C(d.mcs);
    return true;
}

Bits lteDciBuild(const LteDciSi& d, int nRb) {
    Bits b;
    if (d.format == 0) {
        const int nb = ceilLog2((long)nRb * (nRb + 1) / 2);
        put(b, 1, 1); put(b, d.distributed, 1); put(b, (uint32_t)lteRivMake(d.rbStart, d.lCrb, nRb), nb); put(b, (uint32_t)d.mcs, 5); put(b, 0, 3);
        put(b, (d.distributed && nRb >= 50) ? (uint32_t)d.gap : 0u, 1); put(b, (uint32_t)d.rv, 2); put(b, d.nPrb1A == 3 ? 1u : 0u, 2);
        while ((int)b.size() < lteDci1ASize(nRb)) b.push_back(0);
    } else {
        const int step = nRb < 50 ? 2 : 4, np = nVrbGap1(nRb) / step;
        if (nRb >= 50) put(b, (uint32_t)d.gap, 1);
        put(b, (uint32_t)lteRivMake(d.rbStart / step, d.lCrb / step, np), ceilLog2((long)np * (np + 1) / 2));
        put(b, (uint32_t)d.mcs, 5);
    }
    return b;
}

static const int kTbs[27][10] = {
    {16, 32, 56, 88, 120, 152, 176, 208, 224, 256}, {24, 56, 88, 144, 176, 208, 224, 256, 328, 344}, {32, 72, 144, 176, 208, 256, 296, 328, 376, 424},
    {40, 104, 176, 208, 256, 328, 392, 440, 504, 568}, {56, 120, 208, 256, 328, 408, 488, 552, 632, 696}, {72, 144, 224, 328, 424, 504, 600, 680, 776, 872},
    {88, 176, 256, 392, 504, 600, 712, 808, 936, 1032}, {104, 224, 328, 472, 584, 712, 840, 968, 1096, 1224}, {120, 256, 392, 536, 680, 808, 968, 1096, 1256, 1384},
    {136, 296, 456, 616, 776, 936, 1096, 1256, 1416, 1544}, {144, 328, 504, 680, 872, 1032, 1224, 1384, 1544, 1736}, {176, 376, 584, 776, 1000, 1192, 1384, 1608, 1800, 2024},
    {208, 440, 680, 904, 1128, 1352, 1608, 1800, 2024, 2280}, {224, 488, 744, 1000, 1256, 1544, 1800, 2024, 2280, 2536}, {256, 552, 840, 1128, 1416, 1736, 1992, 2280, 2600, 2856},
    {280, 600, 904, 1224, 1544, 1800, 2152, 2472, 2728, 3112}, {328, 632, 968, 1288, 1608, 1928, 2280, 2600, 2984, 3240}, {336, 696, 1064, 1416, 1800, 2152, 2536, 2856, 3240, 3624},
    {376, 776, 1160, 1544, 1992, 2344, 2792, 3112, 3624, 4008}, {408, 840, 1288, 1736, 2152, 2600, 2984, 3496, 3880, 4264}, {440, 904, 1384, 1864, 2344, 2792, 3240, 3752, 4136, 4584},
    {488, 1000, 1480, 1992, 2472, 2984, 3496, 4008, 4584, 4968}, {520, 1064, 1608, 2152, 2664, 3240, 3752, 4264, 4776, 5352}, {552, 1128, 1736, 2280, 2856, 3496, 4008, 4584, 5160, 5736},
    {584, 1192, 1800, 2408, 2984, 3624, 4264, 4968, 5544, 5992}, {616, 1256, 1864, 2536, 3112, 3752, 4392, 5160, 5736, 6200}, {712, 1480, 2216, 2984, 3752, 4392, 5160, 5992, 6712, 7480}};

int lteTbs(int itbs, int nPrb) { return (itbs >= 0 && itbs < 27 && nPrb >= 1 && nPrb <= 10) ? kTbs[itbs][nPrb - 1] : 0; }
int lteTbs1C(int i) {
    static const int t[32] = {40, 56, 72, 120, 136, 144, 176, 208, 224, 256, 280, 296, 328, 336, 392, 488, 552, 600, 632, 696, 776, 840, 904, 1000, 1064, 1128, 1224, 1288, 1384, 1480, 1608, 1736};
    return (i >= 0 && i < 32) ? t[i] : 0;
}

static int rbgSize(int n) { return n <= 10 ? 1 : n <= 26 ? 2 : n <= 63 ? 3 : 4; }

void ltePrbs(const LteDciSi& d, int nRb, std::vector<int>& s0, std::vector<int>& s1) {
    s0.clear(); s1.clear();
    if (!d.distributed) {
        for (int i = 0; i < d.lCrb; i++) { s0.push_back(d.rbStart + i); s1.push_back(d.rbStart + i); }
        return;
    }
    int g1, g2; gapsFor(nRb, g1, g2);
    const int ngap = d.gap ? g2 : g1;
    const int nt = d.gap ? 2 * g2 : nVrbGap1(nRb);
    const int P = rbgSize(nRb);
    const int nrow = (nt + 4 * P - 1) / (4 * P) * P;
    const int nnull = 4 * nrow - nt;
    for (int v = d.rbStart; v < d.rbStart + d.lCrb; v++) {
        const int vt = v % nt, base = nt * (v / nt);
        const int n1 = 2 * nrow * (vt % 2) + vt / 2 + base, n2 = nrow * (vt % 4) + vt / 4 + base;
        int p;
        if (nnull != 0 && vt >= nt - nnull && vt % 2 == 1) p = n1 - nrow;
        else if (nnull != 0 && vt >= nt - nnull && vt % 2 == 0) p = n1 - nrow / 2;
        else if (nnull != 0 && vt < nt - nnull && vt % 4 >= 2) p = n2 - nnull / 2;
        else p = n2;
        const int q = (p - base + nt / 2) % nt + base;
        auto fin = [&](int x) { return x < nt / 2 ? x : x + ngap - nt / 2; };
        s0.push_back(fin(p)); s1.push_back(fin(q));
    }
    std::sort(s0.begin(), s0.end()); std::sort(s1.begin(), s1.end());
}

int ltePbchBits(bool ext) { return ext ? 1728 : 1920; }

static bool syncOrPbch(const LteCellCfg& c, int sf, int lsf, int k) {
    const int N = lteSymbPerSlot(c.extCp);
    if (k < 6 * c.nRb - 36 || k >= 6 * c.nRb + 36) return false;
    if (sf == 0 && lsf >= N && lsf < N + 4) return true;   // PBCH
    if (!c.tdd) return (sf == 0 || sf == 5) && (lsf == N - 2 || lsf == N - 1);
    if ((sf == 0 || sf == 5) && lsf == 2 * N - 1) return true;   // TDD SSS
    if ((sf == 1 || sf == 6) && lsf == 2) return true;          // TDD PSS
    return false;
}

std::vector<std::pair<int, int>> ltePdschRes(const LteCellCfg& c, int sf, int cfi, const std::vector<int>& prb0, const std::vector<int>& prb1) {
    std::vector<std::pair<int, int>> v;
    const int N = lteSymbPerSlot(c.extCp);
    for (int slot = 0; slot < 2; slot++) {
        const std::vector<int>& prb = slot ? prb1 : prb0;
        for (int lsf = slot * N; lsf < (slot + 1) * N; lsf++) {
            if (lsf < lteCtrlSyms(cfi, c.nRb)) continue;
            for (int r : prb) for (int k = 12 * r; k < 12 * r + 12; k++) {
                if (lteCrsAny(c, sf, lsf, k, c.ports)) continue;
                if (syncOrPbch(c, sf, lsf, k)) continue;
                v.push_back({lsf, k});
            }
        }
    }
    return v;
}

} // namespace dect2::cell
