// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// J.83 Annex B: see j83b.h. The clause and figure numbers are of Rec. ITU-T J.83 (12/2007).
#include "dect2/dvbc/j83b.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <numeric>

namespace dect2 {
namespace j83b {

namespace {
// ------------------------------------------------------------------------------------------------ GF(128)
struct Gf {
    uint8_t alog[254] = {}; int log[128] = {};
    Gf() { int v = 1; for (int i = 0; i < 127; i++) { alog[i] = (uint8_t)v; alog[i + 127] = (uint8_t)v; log[v] = i; v <<= 1; if (v & 0x80) v ^= 0x89; } }
};
const Gf& gf() { static const Gf g; return g; }
int mul(int a, int b) { if (!a || !b) return 0; const Gf& G = gf(); return G.alog[G.log[a] + G.log[b]]; }
int pw(int i) { i %= 127; if (i < 0) i += 127; return gf().alog[i]; }

// ------------------------------------------------------------------------------------------------ constellations (figures B.18, B.19)
static const int kLab64[8][8] = {{55,59,23,27,37,47,53,63}, {52,56,20,24,32,42,48,58}, {39,43,7,11,5,15,21,31}, {36,40,4,8,0,10,16,26},
                                 {19,25,3,9,1,13,33,45}, {22,28,6,12,2,14,34,46}, {51,57,35,41,17,29,49,61}, {54,60,38,44,18,30,50,62}};

int lab256(int c, int r) {   // column left to right, row top to bottom; four quadrants of figure B.19
    static const int code[8] = {0, 3, 4, 7, 8, 11, 12, 15};
    if (c >= 8 && r < 8) { const int i = c - 8, j = 7 - r; return code[i] << 4 | code[j]; }
    if (c < 8 && r < 8) { return ((7 - r) * 2 + (c & 1)) << 4 | ((15 - 2 * c) - (r & 1)); }
    if (c < 8 && r >= 8) { static const int h[8] = {14, 13, 10, 9, 6, 5, 2, 1}, l[8] = {1, 2, 5, 6, 9, 10, 13, 14}; return h[c] << 4 | l[r - 8]; }
    const int i = c - 8, j = r - 8; return (2 * j + (i & 1)) << 4 | (2 * i + (j & 1 ? 0 : 1));
}

struct Map {
    int m, n;                       // bits per symbol, points per axis
    std::vector<cf32> pts;          // by label
    float unit;                     // grid step / 2
    std::vector<int> labelAt;       // n * n: by column (x) and row (y from the top)
    int coded1, coded0;             // the bit numbers of the coded bits: U and V
    Map(bool q256) {
        m = q256 ? 8 : 6; n = q256 ? 16 : 8;
        coded1 = q256 ? 4 : 3; coded0 = 0;
        pts.resize(1u << m); labelAt.resize((size_t)n * n);
        double e = 0;
        for (int r = 0; r < n; r++) for (int c = 0; c < n; c++) {
            const double x = 2 * c - (n - 1), y = (n - 1) - 2 * r;
            e += x * x + y * y;
        }
        e /= n * n;
        unit = (float)(1.0 / std::sqrt(e));
        for (int r = 0; r < n; r++) for (int c = 0; c < n; c++) {
            const int l = q256 ? lab256(c, r) : kLab64[r][c];
            labelAt[(size_t)r * n + c] = l;
            pts[(size_t)l] = cf32((2.f * c - (n - 1)) * unit, ((n - 1) - 2.f * r) * unit);
        }
    }
};
const Map& map(bool q256) { static const Map a(false), b(true); return q256 ? b : a; }

// ------------------------------------------------------------------------------------------------ the rate 1/2, 16 state code (G1 = 25, G2 = 37 octal)
inline int bccOut(int st, int b) {     // state: the last four inputs, newest in bit 0. Returns G1 << 1 | G2
    const int r = (st << 1 | b) & 31;
    const int g1 = (r ^ (r >> 2) ^ (r >> 4)) & 1, g2 = (r ^ (r >> 1) ^ (r >> 2) ^ (r >> 3) ^ (r >> 4)) & 1;
    return g1 << 1 | g2;
}
// puncturing [0001;1111]: of 4 input bits the outputs are G2(0), G2(1), G2(2), G1(3), G2(3) in time order
struct BccEnc {
    int st = 0;
    void group(const int in[4], int out[5]) {
        int o0 = bccOut(st, in[0]); st = ((st << 1) | in[0]) & 15;
        int o1 = bccOut(st, in[1]); st = ((st << 1) | in[1]) & 15;
        int o2 = bccOut(st, in[2]); st = ((st << 1) | in[2]) & 15;
        int o3 = bccOut(st, in[3]); st = ((st << 1) | in[3]) & 15;
        out[0] = o0 & 1; out[1] = o1 & 1; out[2] = o2 & 1; out[3] = o3 >> 1; out[4] = o3 & 1;
    }
};

struct Precode { int x = 0, y = 0; void step(int w, int z, int& X, int& Y) { const int d = (x ^ y) & z; X = w ^ x ^ d; Y = z ^ w ^ y ^ d; x = X; y = Y; } };
struct Depre {   // the inverse of the precoder, from the decoded X, Y
    int px = 0, py = 0;
    void step(int X, int Y, int& w, int& z) { z = X ^ Y ^ px ^ py; w = X ^ px ^ (z & (px ^ py)); px = X; py = Y; }
};

// A sliding-window Viterbi decoder for one stream: per group 4 input bits, 5 soft outputs (LLR: positive = bit 1).
struct Vit {
    std::array<float, 16> metric;
    // decisions per step: 16 bits, for state s the survivor bit is decision bit s
    std::vector<uint16_t> dec;
    Vit() { metric.fill(0.f); }
    void reset() { metric.fill(0.f); dec.clear(); }
    inline void step(float g1, float g2, int useG1) {
        std::array<float, 16> nw; uint16_t d = 0;
        for (int s = 0; s < 16; s++) {
            const int p0 = s >> 1, p1 = (s >> 1) | 8;   // predecessors: the bit shifted out is the oldest
            const int b = s & 1;
            const int o0 = bccOut(p0, b), o1 = bccOut(p1, b);
            float m0 = metric[p0], m1 = metric[p1];
            m0 += (o0 & 1 ? g2 : -g2); m1 += (o1 & 1 ? g2 : -g2);
            if (useG1) { m0 += (o0 >> 1 ? g1 : -g1); m1 += (o1 >> 1 ? g1 : -g1); }
            if (m1 > m0) { nw[s] = m1; d |= (uint16_t)(1u << s); } else nw[s] = m0;
        }
        metric = nw;
        dec.push_back(d);
    }
    void group(const float y[5]) { step(0, y[0], 0); step(0, y[1], 0); step(0, y[2], 0); step(y[3], y[4], 1); }
    // decide all but the last `keep` steps; the decisions kept stay in the buffer. Metrics re-normalised.
    void take(int keep, std::vector<uint8_t>& out) {
        const int n = (int)dec.size();
        if (n <= keep) return;
        int s = 0; for (int i = 1; i < 16; i++) if (metric[i] > metric[s]) s = i;
        std::vector<uint8_t> bits((size_t)n);
        for (int i = n - 1; i >= 0; i--) {
            bits[(size_t)i] = (uint8_t)(s & 1);
            const int pb = (dec[(size_t)i] >> s) & 1;
            s = (s >> 1) | (pb << 3);
        }
        out.insert(out.end(), bits.begin(), bits.begin() + (n - keep));
        dec.erase(dec.begin(), dec.begin() + (n - keep));
        const float top = *std::max_element(metric.begin(), metric.end());
        for (auto& m : metric) m -= top;
    }
};

// One group's soft information from 5 received symbols.
inline float llrBit(const Map& m, cf32 z, int bit, float mask[2]) {
    // squared distance to the nearest point with the bit 1 and with the bit 0: a small search over the square around z
    const int n = m.n;
    const float k = 1.f / (2 * m.unit);
    int cx = (int)std::floor(z.real() * k + n / 2.f), cy = (int)std::floor(-z.imag() * k + n / 2.f);
    float d[2] = {1e9f, 1e9f};
    for (int r = std::max(0, cy - 3); r <= std::min(n - 1, cy + 3); r++) for (int c = std::max(0, cx - 3); c <= std::min(n - 1, cx + 3); c++) {
        const int l = m.labelAt[(size_t)r * n + c];
        const float e = std::norm(z - m.pts[(size_t)l]);
        const int b = (l >> bit) & 1;
        if (e < d[b]) d[b] = e;
    }
    mask[0] = d[0]; mask[1] = d[1];
    return d[0] - d[1];
}

constexpr int kMaxBits = 8 * 1024 * 1024;
}   // namespace

int gfMul(int a, int b) { return mul(a, b); }
const std::vector<cf32>& points(bool q256) { return map(q256).pts; }

bool interleaver(int c, int& I, int& J) {
    static const int t[16][2] = {{128, 1}, {128, 1}, {128, 2}, {64, 2}, {128, 3}, {32, 4}, {128, 4}, {16, 8}, {128, 5}, {8, 16}, {128, 6}, {-1, -1}, {128, 7}, {-1, -1}, {128, 8}, {-1, -1}};
    if (c < 0 || c > 15 || t[c][0] < 0) return false;
    I = t[c][0]; J = t[c][1]; return true;
}

// generator with roots alpha^1 .. alpha^5; message order c121 .. c0, then the parity, then the extended symbol
static const std::array<int, 6>& genPoly() {
    static std::array<int, 6> g = [] { std::array<int, 6> p{}; p[0] = 1; for (int i = 1; i <= 5; i++) { std::array<int, 6> q{}; for (int k = 0; k <= i; k++) { q[(size_t)k] = (k < i ? mul(p[(size_t)k], pw(i)) : 0) ^ (k ? p[(size_t)k - 1] : 0); } p = q; } return p; }();
    return g;
}
void rsEncode(uint8_t* b) {
    const auto& g = genPoly();
    int r[5] = {0, 0, 0, 0, 0};     // remainder, r[4] the highest power
    for (int i = 0; i < 122; i++) {
        const int fb = b[i] ^ r[4];
        for (int k = 4; k > 0; k--) r[k] = r[k - 1] ^ mul(fb, g[(size_t)k]);
        r[0] = mul(fb, g[0]);
    }
    for (int k = 0; k < 5; k++) b[122 + k] = (uint8_t)r[4 - k];
    int v = 0;   // the extended symbol: c(alpha^6), transmitted index n has power 126 - n
    for (int n = 0; n < 127; n++) v ^= mul(b[n], pw(6 * (126 - n)));
    b[127] = (uint8_t)v;
}
int rsDecode(uint8_t* b) {
    int S[7]; bool any = false;
    for (int j = 1; j <= 6; j++) { int v = 0; for (int n = 0; n < 127; n++) v ^= mul(b[n], pw(j * (126 - n))); S[j] = v; }
    S[6] ^= b[127];
    for (int j = 1; j <= 6; j++) if (S[j]) any = true;
    if (!any) return 0;
    // only the extended symbol wrong
    if (!S[1] && !S[2] && !S[3] && !S[4] && !S[5]) { b[127] ^= (uint8_t)S[6]; return 1; }
    // Berlekamp-Massey over the six syndromes, or (second pass: the extended symbol is taken as wrong too) over the first five, which see no error there
    for (int pass = 0; pass < 2; pass++) {
    const int nSyn = pass == 0 ? 6 : 5, maxL = pass == 0 ? 3 : 2;
    int C[5] = {1, 0, 0, 0, 0}, B[5] = {1, 0, 0, 0, 0}, L = 0, m = 1, bb = 1;
    for (int n = 0; n < nSyn; n++) {
        int d = S[n + 1];
        for (int i = 1; i <= L; i++) d ^= mul(C[i], S[n + 1 - i]);
        if (d == 0) { m++; continue; }
        int T[5]; memcpy(T, C, sizeof C);
        const int coef = mul(d, pw(127 - gf().log[bb]));
        for (int i = 0; i + m <= 4; i++) C[i + m] ^= mul(coef, B[i]);
        if (2 * L <= n) { L = n + 1 - L; memcpy(B, T, sizeof B); bb = d; m = 1; } else m++;
    }
    if (L > maxL) continue;
    int rootsN[3], rootsP[3], cnt = 0;
    for (int p = 0; p < 127; p++) {
        int v = 1;
        for (int i = 1; i <= L; i++) v ^= mul(C[i], pw((127 - p) * i));   // C(X^-1) with X = alpha^p
        if (v == 0) { if (cnt < 3) { rootsN[cnt] = 126 - p; rootsP[cnt] = p; } cnt++; }
    }
    if (cnt != L) continue;
    // Forney with the syndromes S1.. (first consecutive root 1): solve the small linear system directly (L <= 3)
    int e[3]; bool bad = false;
    {
        int A[3][4];
        for (int r = 0; r < L; r++) { for (int c = 0; c < L; c++) A[r][c] = pw(rootsP[c] * (r + 1)); A[r][L] = S[r + 1]; }
        for (int c = 0; c < L; c++) {
            int piv = -1; for (int r = c; r < L; r++) if (A[r][c]) { piv = r; break; }
            if (piv < 0) { bad = true; break; }
            if (piv != c) for (int k = 0; k <= L; k++) std::swap(A[piv][k], A[c][k]);
            const int inv = pw(127 - gf().log[A[c][c]]);
            for (int k = c; k <= L; k++) A[c][k] = mul(A[c][k], inv);
            for (int r = 0; r < L; r++) if (r != c && A[r][c]) { const int f = A[r][c]; for (int k = c; k <= L; k++) A[r][k] ^= mul(f, A[c][k]); }
        }
        for (int i = 0; i < L; i++) e[i] = A[i][L];
    }
    if (bad) continue;
    uint8_t t[128]; memcpy(t, b, 128);
    for (int i = 0; i < L; i++) t[rootsN[i]] ^= (uint8_t)e[i];
    // check all six syndromes (this also catches an error in the extended symbol that went with others)
    bool okS = true;
    for (int j = 1; j <= 5 && okS; j++) { int v = 0; for (int n = 0; n < 127; n++) v ^= mul(t[n], pw(j * (126 - n))); if (v) okS = false; }
    if (!okS) continue;
    { int v = 0; for (int n = 0; n < 127; n++) v ^= mul(t[n], pw(6 * (126 - n))); t[127] = (uint8_t)v; }
    memcpy(b, t, 128);
    return L;
    }
    return -1;
}

namespace {
// The randomizer: f(x) = x^3 + x + alpha^3, loaded with all ones; the output of the top register goes over the symbol, then the register steps
struct Randomizer {
    int r[3] = {0x7F, 0x7F, 0x7F};
    void init() { r[0] = r[1] = r[2] = 0x7F; }
    int next() { const int o = r[2]; const int nr2 = r[1], nr1 = r[0] ^ r[2], nr0 = mul(r[2], pw(3)); r[2] = nr2; r[1] = nr1; r[0] = nr0; return o; }
};

// convolutional interleaver over symbols, commutator reset at each frame start
struct ConvInt {
    int I = 0, J = 0, pos = 0; bool de = false;
    std::vector<std::deque<uint8_t>> lines;
    void configure(int II, int JJ, bool deint) { I = II; J = JJ; de = deint; lines.assign((size_t)I, {}); for (int i = 0; i < I; i++) lines[(size_t)i].assign((size_t)((deint ? I - 1 - i : i) * J), 0); pos = 0; }
    void frameStart() { pos = 0; }
    uint8_t step(uint8_t v) {
        auto& q = lines[(size_t)pos]; pos = (pos + 1) % I;
        if (q.empty()) return v;
        q.push_back(v); const uint8_t o = q.front(); q.pop_front(); return o;
    }
};
}   // namespace

// ------------------------------------------------------------------------------------------------ encoder

struct J83bEncoder::Impl {
    EncConfig c; const Map& m;
    std::vector<uint8_t> tsBits;
    std::vector<uint8_t> rsSyms;
    ConvInt ilv; Randomizer rnd; Precode pre; BccEnc a, b;
    std::vector<cf32> out;
    std::vector<uint8_t> pending;  // bits awaiting a whole trellis group (64-QAM: frames do not end on a group)
    int bI = 128, bJ = 1;
    explicit Impl(const EncConfig& cc) : c(cc), m(map(cc.qam256)) { interleaver(c.control, bI, bJ); ilv.configure(bI, bJ, false); }

    void trellisGroup64(const uint8_t* g) {
        // symbol bits MSB first: A10 A8 A7 A5 A4 A2 A1 | A9 A6 A3 A0 A13 A12 A11 | B ... (figure B.13)
        static const int ord[14] = {10, 8, 7, 5, 4, 2, 1, 9, 6, 3, 0, 13, 12, 11};
        int A[14], B[14];
        for (int i = 0; i < 14; i++) { A[ord[i]] = g[i]; B[ord[i]] = g[14 + i]; }
        const int wA[4] = {A[0], A[3], A[6], A[9]}, zB[4] = {B[0], B[3], B[6], B[9]};
        int X[4], Y[4];
        for (int i = 0; i < 4; i++) pre.step(wA[i], zB[i], X[i], Y[i]);
        int U[5], V[5]; a.group(X, U); b.group(Y, V);
        // uncoded per symbol: row above is (A2,A5,A8,A11,A13), below (A1,A4,A7,A10,A12)
        static const int up[5] = {2, 5, 8, 11, 13}, lo[5] = {1, 4, 7, 10, 12};
        for (int t = 0; t < 5; t++) {
            int aH = A[up[t]], aL = A[lo[t]], bH = B[up[t]], bL = B[lo[t]], u = U[t], v = V[t];
            if (c.swapAB) { std::swap(aH, bH); std::swap(aL, bL); std::swap(u, v); }
            const int l = aH << 5 | aL << 4 | u << 3 | bH << 2 | bL << 1 | v;
            out.push_back(m.pts[(size_t)l]);
        }
    }
    void trellisGroup256(const uint8_t* g, bool sync, const uint8_t* s) {
        // bit order of figure B.15; the 8 bits at A0 B0 A4 B4 A8 B8 A12 B12 are the coded ones, or sync bits in a sync group
        static const char order[38][2] = {{0,0},{1,0},{0,1},{0,2},{0,3},{1,1},{1,2},{1,3},{0,4},{1,4},{0,5},{0,6},{0,7},{1,5},{1,6},{1,7},{0,8},{1,8},{0,9},{0,10},{0,11},{1,9},{1,10},{1,11},{0,12},{1,12},{0,13},{0,14},{0,15},{1,13},{1,14},{1,15},{0,16},{0,17},{0,18},{1,16},{1,17},{1,18}};
        int V_[2][19]{};
        int p = 0;
        for (int i = 0; i < 38; i++) { const int st = order[i][0], k = order[i][1]; const bool c0 = k == 0 || k == 4 || k == 8 || k == 12;
            if (sync && c0) { continue; }
            V_[st][k] = g[p++]; }
        if (sync) { int sb = 0; static const int ka[4] = {0, 4, 8, 12}; for (int i = 0; i < 4; i++) { V_[0][ka[i]] = s[sb++]; V_[1][ka[i]] = s[sb++]; } }
        const int wA[4] = {V_[0][0], V_[0][4], V_[0][8], V_[0][12]}, zB[4] = {V_[1][0], V_[1][4], V_[1][8], V_[1][12]};
        int X[4], Y[4];
        for (int i = 0; i < 4; i++) pre.step(wA[i], zB[i], X[i], Y[i]);
        int U[5], V[5]; a.group(X, U); b.group(Y, V);
        static const int r3[5] = {3, 7, 11, 15, 18}, r2[5] = {2, 6, 10, 14, 17}, r1[5] = {1, 5, 9, 13, 16};
        for (int t = 0; t < 5; t++) {
            int a3 = V_[0][r3[t]], a2 = V_[0][r2[t]], a1 = V_[0][r1[t]], b3 = V_[1][r3[t]], b2 = V_[1][r2[t]], b1 = V_[1][r1[t]], u = U[t], v = V[t];
            if (c.swapAB) { std::swap(a3, b3); std::swap(a2, b2); std::swap(a1, b1); std::swap(u, v); }
            const int l = a3 << 7 | a2 << 6 | a1 << 5 | u << 4 | b3 << 3 | b2 << 2 | b1 << 1 | v;
            out.push_back(m.pts[(size_t)l]);
        }
    }
    void flushFrames() {
        const int blocks = c.qam256 ? 88 : 60;
        const size_t need = (size_t)blocks * 122 * 7;
        while (tsBits.size() >= need) {
            std::vector<uint8_t> sy((size_t)blocks * 128);
            for (int b_ = 0; b_ < blocks; b_++) {
                uint8_t* blk = &sy[(size_t)b_ * 128];
                for (int i = 0; i < 122; i++) { int v = 0; for (int k = 0; k < 7; k++) v = v << 1 | tsBits[(size_t)b_ * 854 + (size_t)i * 7 + (size_t)k]; blk[i] = (uint8_t)v; }
                rsEncode(blk);
            }
            tsBits.erase(tsBits.begin(), tsBits.begin() + (ptrdiff_t)need);
            ilv.frameStart(); rnd.init();
            for (auto& v : sy) { v = ilv.step(v); v ^= (uint8_t)rnd.next(); }
            std::vector<uint8_t> bits; bits.reserve(sy.size() * 7 + 42);
            for (uint8_t v : sy) for (int k = 6; k >= 0; k--) bits.push_back((v >> k) & 1);
            if (!c.qam256) {
                static const uint8_t fs[4] = {0x75, 0x2C, 0x0D, 0x6C};
                for (uint8_t v : fs) for (int k = 6; k >= 0; k--) bits.push_back((v >> k) & 1);
                for (int k = 3; k >= 0; k--) bits.push_back((c.control >> k) & 1);
                for (int k = 0; k < 10; k++) bits.push_back(0);
                pending.insert(pending.end(), bits.begin(), bits.end());
                size_t pos = 0;
                while (pending.size() - pos >= 28) { trellisGroup64(pending.data() + pos, 0); pos += 28; }
                pending.erase(pending.begin(), pending.begin() + (ptrdiff_t)pos);
            } else {
                size_t pos = 0;
                for (int g = 0; g < 2071; g++) { trellisGroup256(bits.data() + pos, false, nullptr); pos += 38; }
                uint8_t sb[40] = {};
                static const uint32_t w = 0x71E84DD4;
                for (int k = 0; k < 32; k++) sb[k] = (w >> (31 - k)) & 1;
                for (int k = 0; k < 4; k++) sb[32 + k] = (c.control >> (3 - k)) & 1;
                for (int g = 0; g < 5; g++) { trellisGroup256(bits.data() + pos, true, sb + g * 8); pos += 30; }
            }
        }
    }
    void trellisGroup64(const uint8_t* g, int) { trellisGroup64(g); }
};

J83bEncoder::J83bEncoder(const EncConfig& c) : p_(new Impl(c)) {}
J83bEncoder::~J83bEncoder() = default;
void J83bEncoder::push(const uint8_t* pkt) {
    Impl& I = *p_;
    uint8_t sl[188]; memcpy(sl, pkt, 188);
    // the checksum of the 187 bytes before: LFSR 1/g(x), g(x) = 1 + x + x^5 + x^6 + x^8, not verified against the figure: the receiver finds the packets without it
    {
        int st = 0;
        for (int i = 1; i < 188; i++) for (int k = 7; k >= 0; k--) { const int fb = ((sl[i] >> k) & 1) ^ ((st >> 7) & 1); st = ((st << 1) & 0xFF) ^ (fb ? 0x63 : 0); }
        sl[0] = (uint8_t)(st ^ 0x67);
    }
    for (int i = 0; i < 188; i++) for (int k = 7; k >= 0; k--) I.tsBits.push_back((sl[i] >> k) & 1);
    I.flushFrames();
}
void J83bEncoder::take(std::vector<cf32>& s) { s.insert(s.end(), p_->out.begin(), p_->out.end()); p_->out.clear(); }

// ------------------------------------------------------------------------------------------------ decoder

struct J83bDecoder::Impl {
    bool q256 = false;
    DecStats st;
    std::function<void(const uint8_t*, size_t)> cb;
    int phase = 0; bool swap_ = false;
    std::vector<cf32> sbuf;
    Vit vA, vB; std::vector<uint8_t> xA, xB; Depre dep;
    std::vector<cf32> lag;          // symbols waiting for their coded bits
    bool locked = false;
    uint64_t bitPos = 0;
    // frame sync
    std::vector<uint8_t> fbits;     // serial data bits (64) / per group bits (256)
    std::vector<uint8_t> frameBits;
    bool frameSynced = false;
    int frameLenBits;
    ConvInt dei; Randomizer rnd;
    std::vector<uint8_t> tsBits;
    int packetOffset = -1;          // bit offset of the sync slot in tsBits, after alignment
    std::vector<uint8_t> align;     // bits collected for the alignment
    BccEnc reA, reB;
    int badFrames = 0;
    bool frameBad = false;

    Impl() { configure(false); }
    void configure(bool q) { q256 = q; reset(); }
    void reset() {
        st = DecStats(); locked = false; phase = 0; swap_ = false; sbuf.clear(); vA.reset(); vB.reset(); xA.clear(); xB.clear(); dep = Depre(); lag.clear(); bitPos = 0; fbits.clear(); frameBits.clear(); frameSynced = false; tsBits.clear(); packetOffset = -1; align.clear(); reA = BccEnc(); reB = BccEnc(); badFrames = 0; dei = ConvInt();
        frameLenBits = q256 ? 0 : 53802; rnd.init();
    }
    // Soft values of one group
    void softGroup(const cf32* s, float ya[5], float yb[5], bool swp) const {
        const Map& m = map(q256);
        float mk[2];
        for (int t = 0; t < 5; t++) {
            float a = llrBit(m, s[t], m.coded1, mk), b = llrBit(m, s[t], m.coded0, mk);
            if (swp) std::swap(a, b);
            ya[t] = a; yb[t] = b;
        }
    }
    // Nearest point given the two coded bits: returns the label
    int pick(cf32 z, int u, int v, bool swp) const {
        const Map& m = map(q256);
        if (swp) std::swap(u, v);
        const int n = m.n; const float k = 1.f / (2 * m.unit);
        int cx = (int)std::floor(z.real() * k + n / 2.f), cy = (int)std::floor(-z.imag() * k + n / 2.f), best = -1; float bd = 1e9f;
        for (int r = std::max(0, cy - 3); r <= std::min(n - 1, cy + 3); r++) for (int c = std::max(0, cx - 3); c <= std::min(n - 1, cx + 3); c++) {
            const int l = m.labelAt[(size_t)r * n + c];
            if (((l >> m.coded1) & 1) != u || ((l >> m.coded0) & 1) != v) continue;
            const float e = std::norm(z - m.pts[(size_t)l]);
            if (e < bd) { bd = e; best = l; }
        }
        if (best < 0) { for (int l = 0; l < (1 << m.m); l++) if (((l >> m.coded1) & 1) == u && ((l >> m.coded0) & 1) == v) { const float e = std::norm(z - m.pts[(size_t)l]); if (e < bd) { bd = e; best = l; } } }
        return best;
    }

    // from symbols to serial bits. Groups go through the two Viterbi decoders; the bits decided so far are combined with their waiting symbols.
    void groupsToBits(const cf32* sym, size_t nGroups, std::vector<uint8_t>& serial, bool flush = false) {
        const size_t kOV = 40;
        for (size_t g = 0; g < nGroups; g++) {
            float ya[5], yb[5];
            softGroup(sym + g * 5, ya, yb, swap_);
            vA.group(ya); vB.group(yb);
            lag.insert(lag.end(), sym + g * 5, sym + g * 5 + 5);
        }
        std::vector<uint8_t> outA, outB;
        vA.take(flush ? 0 : (int)kOV * 4, outA); vB.take(flush ? 0 : (int)kOV * 4, outB);
        const size_t ng = outA.size() / 4;
        size_t ptr = 0;
        for (size_t g = 0; g < ng; g++, ptr += 5) {
            int X[4], Y[4], W[4], Z[4];
            for (int i = 0; i < 4; i++) { X[i] = outA[g * 4 + (size_t)i]; Y[i] = outB[g * 4 + (size_t)i]; dep.step(X[i], Y[i], W[i], Z[i]); }
            int U[5], V[5]; reA.group(X, U); reB.group(Y, V);
            const Map& m = map(q256);
            if (!q256) {
                int aU[14] = {}, bU[14] = {};
                static const int up[5] = {2, 5, 8, 11, 13}, lo[5] = {1, 4, 7, 10, 12};
                for (int t = 0; t < 5; t++) {
                    const int l = pick(lag[ptr + (size_t)t], U[t], V[t], swap_);
                    int a2 = (l >> 5) & 1, a1 = (l >> 4) & 1, b2 = (l >> 2) & 1, b1 = (l >> 1) & 1;
                    if (swap_) { std::swap(a2, b2); std::swap(a1, b1); }
                    aU[up[t]] = a2; aU[lo[t]] = a1; bU[up[t]] = b2; bU[lo[t]] = b1;
                }
                aU[0] = W[0]; aU[3] = W[1]; aU[6] = W[2]; aU[9] = W[3];
                bU[0] = Z[0]; bU[3] = Z[1]; bU[6] = Z[2]; bU[9] = Z[3];
                static const int ord[14] = {10, 8, 7, 5, 4, 2, 1, 9, 6, 3, 0, 13, 12, 11};
                for (int i = 0; i < 14; i++) serial.push_back((uint8_t)aU[ord[i]]);
                for (int i = 0; i < 14; i++) serial.push_back((uint8_t)bU[ord[i]]);
            } else {
                int A[19] = {}, B[19] = {};
                static const int r3[5] = {3, 7, 11, 15, 18}, r2[5] = {2, 6, 10, 14, 17}, r1[5] = {1, 5, 9, 13, 16};
                for (int t = 0; t < 5; t++) {
                    const int l = pick(lag[ptr + (size_t)t], U[t], V[t], swap_);
                    int a3 = (l >> 7) & 1, a2 = (l >> 6) & 1, a1 = (l >> 5) & 1, b3 = (l >> 3) & 1, b2 = (l >> 2) & 1, b1 = (l >> 1) & 1;
                    if (swap_) { std::swap(a3, b3); std::swap(a2, b2); std::swap(a1, b1); }
                    A[r3[t]] = a3; A[r2[t]] = a2; A[r1[t]] = a1; B[r3[t]] = b3; B[r2[t]] = b2; B[r1[t]] = b1;
                }
                A[0] = W[0]; A[4] = W[1]; A[8] = W[2]; A[12] = W[3];
                B[0] = Z[0]; B[4] = Z[1]; B[8] = Z[2]; B[12] = Z[3];
                static const char order[38][2] = {{0,0},{1,0},{0,1},{0,2},{0,3},{1,1},{1,2},{1,3},{0,4},{1,4},{0,5},{0,6},{0,7},{1,5},{1,6},{1,7},{0,8},{1,8},{0,9},{0,10},{0,11},{1,9},{1,10},{1,11},{0,12},{1,12},{0,13},{0,14},{0,15},{1,13},{1,14},{1,15},{0,16},{0,17},{0,18},{1,16},{1,17},{1,18}};
                for (int i = 0; i < 38; i++) serial.push_back((uint8_t)(order[i][0] ? B[order[i][1]] : A[order[i][1]]));
            }
            (void)m;
        }
        lag.erase(lag.begin(), lag.begin() + (ptrdiff_t)ptr);
    }

    // Finds the sync trailer in the serial bits. Returns the bit index just after the trailer of the second occurrence, -1 if none; control and spacing are checked.
    long findTrailer64(const std::vector<uint8_t>& b, int& control) const {
        static const uint32_t pat = 0x752C0D6C >> 0;   // the 28 bits: 7-bit symbols 75 2C 0D 6C packed
        uint32_t want = 0; { static const uint8_t fs[4] = {0x75, 0x2C, 0x0D, 0x6C}; for (uint8_t v : fs) want = want << 7 | (v & 0x7F); }
        (void)pat;
        uint32_t reg = 0; const uint32_t mask = (1u << 28) - 1;
        const size_t n = b.size();
        for (size_t i = 0; i < n; i++) {
            reg = (reg << 1 | b[i]) & mask;
            if (i + 1 >= 28 && reg == want) {
                const size_t afterFirst = i + 1;
                if (afterFirst + 53802 + 14 <= n) {
                    uint32_t r2 = 0; for (size_t k = 0; k < 28; k++) r2 = (r2 << 1 | b[afterFirst + 53802 - 28 + k]) & mask;
                    if (r2 == want) { int c = 0; for (int k = 0; k < 4; k++) c = c << 1 | b[afterFirst + 53802 + (size_t)k]; control = c; return (long)(afterFirst + 53802 + 14); }
                }
            }
        }
        return -1;
    }

    bool trial(const cf32* sym, size_t n) {
        sbuf.insert(sbuf.end(), sym, sym + n);
        const size_t need = q256 ? 22000 : 26000;     // symbols: two frames and a bit
        if (sbuf.size() < need) return false;
        for (int sw = 0; sw < 2; sw++) for (int ph = 0; ph < 5; ph++) {
            swap_ = sw != 0; phase = ph;
            vA.reset(); vB.reset(); dep = Depre(); reA = BccEnc(); reB = BccEnc(); lag.clear();
            std::vector<uint8_t> ser;
            const size_t ng = (sbuf.size() - (size_t)ph) / 5;
            groupsToBits(sbuf.data() + ph, ng, ser, true);
            int ctrl = -1;
            if (!q256) {
                long after = findTrailer64(ser, ctrl);
                if (after < 0) continue;
                int II, JJ;
                if (!interleaver(ctrl, II, JJ)) continue;
                dei.configure(II, JJ, true); st.control = ctrl; st.I = II; st.J = JJ; st.groupPhase = ph; st.swapAB = swap_;
                rnd.init(); dei.frameStart(); tsBits.clear(); packetOffset = -1; align.clear(); badFrames = 0;
                fbits.assign(ser.begin() + after, ser.end());
                locked = true; st.locked = true;
                sbuf.erase(sbuf.begin(), sbuf.begin() + (ptrdiff_t)(ng * 5 + (size_t)ph));
                drain64();
                // the decoder keeps running on symbols not yet seen by the decoders: the two Viterbi states are consistent with the groups fed, so go on
                return true;
            } else {
                // every group's sync-position bits: A0, B0, A4, B4, A8, B8, A12, B12
                const size_t perG = 38;
                if (ser.size() < perG * 2076 * 2) continue;
                static const int pos[8] = {0, 1, 8, 9, 16, 17, 24, 25};
                std::vector<uint8_t> gb(ser.size() / perG * 8);
                for (size_t g = 0; g * perG < ser.size(); g++) for (int k = 0; k < 8; k++) gb[g * 8 + (size_t)k] = ser[g * perG + (size_t)pos[k]];
                static const uint32_t want = 0x71E84DD4;
                bool found = false;
                uint32_t r = 0; size_t endG = 0;
                for (size_t bi = 0; bi < gb.size(); bi++) {
                    r = (r << 1) | gb[bi];
                    if (bi >= 39 && (r & 0xFFFFFFFF) == want && (bi + 1) % 8 == 0 ? false : false) {}
                    if (bi >= 39 && (bi + 1 - 32) % 8 == 0) {
                        uint32_t rr = 0; for (size_t k = 0; k < 32; k++) rr = rr << 1 | gb[bi + 1 - 32 + k];
                        if (rr == want && (bi + 1 + 8) % 40 != (size_t)-1) {
                            // the trailer is the last 40 of 5 groups; the next group after its fifth starts a frame
                            const size_t grpStart = (bi + 1 - 32) / 8;
                            if (grpStart + 5 + 2076 + 5 > ser.size() / perG) break;
                            uint32_t r2 = 0; for (size_t k = 0; k < 32; k++) r2 = r2 << 1 | gb[(grpStart + 2076) * 8 + k];
                            if (r2 == want) { endG = grpStart + 5; int c = 0; for (int k = 0; k < 4; k++) c = c << 1 | gb[grpStart * 8 + 32 + (size_t)k]; ctrl = c; found = true; break; }
                        }
                    }
                }
                if (!found) continue;
                int II, JJ;
                if (!interleaver(ctrl, II, JJ)) continue;
                dei.configure(II, JJ, true); st.control = ctrl; st.I = II; st.J = JJ; st.groupPhase = ph; st.swapAB = swap_;
                rnd.init(); dei.frameStart(); tsBits.clear(); packetOffset = -1; align.clear(); badFrames = 0;
                fbits.assign(ser.begin() + (ptrdiff_t)(endG * perG), ser.end());
                locked = true; st.locked = true;
                sbuf.erase(sbuf.begin(), sbuf.begin() + (ptrdiff_t)(ng * 5 + (size_t)ph));
                drain256();
                return true;
            }
        }
        // keep the end of the buffer for the next look
        if (sbuf.size() > 60000) sbuf.erase(sbuf.begin(), sbuf.end() - 40000);
        locked = false;
        return false;
    }

    void symbolsToTs(std::vector<uint8_t>& sy) {   // the symbols of a frame, after the trailer was taken off
        rnd.init(); dei.frameStart();
        for (auto& v : sy) { v ^= (uint8_t)rnd.next(); v = dei.step(v); }
        const int blocks = (int)sy.size() / 128;
        for (int b = 0; b < blocks; b++) {
            uint8_t* blk = &sy[(size_t)b * 128];
            const int r = rsDecode(blk);
            if (r < 0) st.rsFailed++; else if (r == 0) st.rsClean++; else { st.rsCorrected++; st.correctedSymbols += (uint32_t)r; }
            if (r < 0) frameBad = true;
        }
        // while the packets are not aligned, keep only stretches that decoded completely (the warm-up of the interleaver and the first frames are garbage or zeros)
        if (packetOffset < 0 && frameBad) { tsBits.clear(); frameBad = false; st.frames++; return; }
        frameBad = false;
        for (int b = 0; b < blocks; b++) { const uint8_t* blk = &sy[(size_t)b * 128]; for (int i = 0; i < 122; i++) for (int k = 6; k >= 0; k--) tsBits.push_back((blk[i] >> k) & 1); }
        st.frames++;
        deliver();
    }
    void deliver() {
        if (packetOffset < 0) {
            // the packet position from the content (a sync slot every 1504 bits): the offset whose packets have the most repeated PIDs and a consistent continuity counter
            if (tsBits.size() < 1504 * 600) return;
            int best = -1, score = 0;
            for (int off = 0; off < 1504; off++) {
                int lastCc[8192]; std::fill(lastCc, lastCc + 8192, -1);
                int good = 0;
                for (int p = 0; p < 400; p++) {
                    const size_t o = (size_t)off + (size_t)p * 1504 + 8;
                    if (o + 24 > tsBits.size()) break;
                    int v = 0; for (int k = 0; k < 24; k++) v = v << 1 | tsBits[o + (size_t)k];
                    if ((v >> 23) & 1) continue;                    // transport_error_indicator
                    const int pid = (v >> 8) & 0x1FFF;
                    if (pid == 0x1FFF || pid == 0) continue;        // null packets, and zero stretches, say nothing
                    const int cc = v & 15;
                    if (((v >> 4) & 3) == 0) { lastCc[pid] = cc; continue; }    // no payload: reserved value
                    if (lastCc[pid] >= 0 && ((lastCc[pid] + 1) & 15) == cc) good++;
                    lastCc[pid] = cc;
                }
                if (good > score) { score = good; best = off; }
            }
            if (best < 0 || score < 40) { if (tsBits.size() > 1504 * 1200) tsBits.erase(tsBits.begin(), tsBits.begin() + 1504 * 300); return; }
            packetOffset = best;
        }
        size_t pos = (size_t)packetOffset;
        const size_t avail = tsBits.size();
        std::vector<uint8_t> out;
        while (pos + 1504 <= avail) {
            uint8_t pk[188];
            pk[0] = 0x47;
            for (int by = 1; by < 188; by++) { int v = 0; for (int k = 0; k < 8; k++) v = v << 1 | tsBits[pos + 8 * (size_t)by + (size_t)k]; pk[by] = (uint8_t)v; }
            pos += 1504;
            out.insert(out.end(), pk, pk + 188);
            st.packets++;
        }
        tsBits.erase(tsBits.begin(), tsBits.begin() + (ptrdiff_t)pos);
        packetOffset = 0;
        if (!out.empty() && cb) cb(out.data(), out.size() / 188);
    }
    void drain64() {
        size_t pos = 0;
        const size_t frame = 53802, dataSyms = 7680;
        while (fbits.size() - pos >= frame) {
            std::vector<uint8_t> sy(dataSyms);
            for (size_t i = 0; i < dataSyms; i++) { int v = 0; for (int k = 0; k < 7; k++) v = v << 1 | fbits[pos + i * 7 + (size_t)k]; sy[i] = (uint8_t)v; }
            uint32_t r = 0; for (int k = 0; k < 28; k++) r = r << 1 | fbits[pos + dataSyms * 7 + (size_t)k];
            static const uint32_t want = (0x75u << 21) | (0x2Cu << 14) | (0x0Du << 7) | 0x6Cu;
            if (r != want) { st.framesBad++; badFrames++; } else badFrames = 0;
            pos += frame;
            symbolsToTs(sy);
        }
        fbits.erase(fbits.begin(), fbits.begin() + (ptrdiff_t)pos);
    }
    void drain256() {
        const size_t groupBits = 38; size_t pos = 0;
        const size_t frameGroups = 2076;
        while (fbits.size() - pos >= frameGroups * groupBits) {
            std::vector<uint8_t> bits; bits.reserve(11264 * 7);
            for (size_t g = 0; g < 2071; g++) bits.insert(bits.end(), fbits.begin() + (ptrdiff_t)(pos + g * groupBits), fbits.begin() + (ptrdiff_t)(pos + g * groupBits + groupBits));
            static const int syncPos[8] = {0, 1, 8, 9, 16, 17, 24, 25};
            uint8_t sb[40];
            for (size_t g = 2071; g < 2076; g++) {
                const uint8_t* b = &fbits[pos + g * groupBits];
                int sk = 0;
                for (int i = 0; i < 38; i++) { bool isS = false; for (int k = 0; k < 8; k++) if (syncPos[k] == i) isS = true; if (isS) sb[(g - 2071) * 8 + (size_t)sk++] = b[i]; else bits.push_back(b[i]); }
            }
            uint32_t r = 0; for (int k = 0; k < 32; k++) r = r << 1 | sb[k];
            if (r != 0x71E84DD4u) { st.framesBad++; badFrames++; } else badFrames = 0;
            pos += frameGroups * groupBits;
            std::vector<uint8_t> sy(11264);
            for (size_t i = 0; i < 11264; i++) { int v = 0; for (int k = 0; k < 7; k++) v = v << 1 | bits[i * 7 + (size_t)k]; sy[i] = (uint8_t)v; }
            symbolsToTs(sy);
        }
        fbits.erase(fbits.begin(), fbits.begin() + (ptrdiff_t)pos);
    }
    void feed(const cf32* sym, size_t n) {
        if (!locked) return;
        sbuf.insert(sbuf.end(), sym, sym + n);
        const size_t ng = sbuf.size() / 5;
        if (ng == 0) return;
        groupsToBits(sbuf.data(), ng, fbits);
        sbuf.erase(sbuf.begin(), sbuf.begin() + (ptrdiff_t)(ng * 5));
        if (q256) drain256(); else drain64();
    }
};

J83bDecoder::J83bDecoder() : p_(new Impl) {}
J83bDecoder::~J83bDecoder() = default;
void J83bDecoder::configure(bool q) { p_->configure(q); }
void J83bDecoder::reset() { p_->reset(); }
bool J83bDecoder::trial(const cf32* s, size_t n) { return p_->trial(s, n); }
void J83bDecoder::feed(const cf32* s, size_t n) { p_->feed(s, n); }
void J83bDecoder::setPacketCallback(std::function<void(const uint8_t*, size_t)> cb) { p_->cb = std::move(cb); }
const DecStats& J83bDecoder::stats() const { return p_->st; }

} // namespace j83b
} // namespace dect2
