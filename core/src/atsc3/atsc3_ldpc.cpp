// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/diag.h"
#include "atsc3_ldpc.h"
#include "dect2/dsp/simd.h"
#include "dect2/dsp/minsum_corr.h"
#include "dect2/dsp/ldpc.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>

namespace dect2 {
namespace atsc3 {

namespace {
// Table 6.6: M1, M2, Q1, Q2 for Type A; Table 6.7: Q for Type B (Ninner = 16200)
struct TypeA { int rate, m1, m2, q1, q2; };
const TypeA kTypeA16[] = {{2, 3240, 10800, 9, 30}, {3, 1080, 11880, 3, 33}, {4, 1080, 10800, 3, 30}, {5, 720, 10080, 2, 28}};
const TypeA kTypeA64[] = {{2, 1800, 54360, 5, 151}, {3, 1800, 50040, 5, 139}, {4, 1800, 45720, 5, 127}, {5, 1440, 41760, 4, 116}, {7, 1080, 33480, 3, 93}};
const int kQb16[14] = {0, 0, 0, 0, 0, 0, 27, 24, 21, 18, 15, 12, 9, 6};
const int kQb64[14] = {0, 0, 0, 0, 0, 0, 108, 0, 84, 72, 60, 48, 36, 24};
}

Ldpc::Ldpc(int rate15, int n) : rate_(rate15), n_(n) {
    if (rate15 < 2 || rate15 > 13 || (n != 16200 && n != 64800)) {
        DIAG_DEBUG_EVERY(128805, 10.0, "An ATSC 3.0 LDPC code was asked for with %d inner bits and rate %d/15, which is not defined, so it is not built.", n, rate15);
        return;
    }
    k_ = n / 15 * rate15;
    m_ = n - k_;
    if (n == 16200) { for (auto& t : kTypeA16) if (t.rate == rate15) { typeA_ = true; m1_ = t.m1; m2_ = t.m2; q1_ = t.q1; q2_ = t.q2; } }
    else { for (auto& t : kTypeA64) if (t.rate == rate15) { typeA_ = true; m1_ = t.m1; m2_ = t.m2; q1_ = t.q1; q2_ = t.q2; } }
    if (!typeA_) qB_ = n == 16200 ? kQb16[rate15] : kQb64[rate15];
    const int* lens; int nrows;
    const int* tab = n == 16200 ? ldpcTable16200(rate15, &lens, &nrows) : ldpcTable64800(rate15, &lens, &nrows);
    int pos = 0;
    for (int r = 0; r < nrows; r++) {
        rows_.emplace_back(tab + pos, tab + pos + lens[r]);
        pos += lens[r];
    }
    buildGraph();
}

int Ldpc::accAddr(int x, int m) const {
    if (typeA_) return x < m1_ ? (x + m * q1_) % m1_ : m1_ + (x - m1_ + m * q2_) % m2_;
    return (x + m * qB_) % m_;
}

void Ldpc::encode(std::vector<uint8_t>& bits) const {
    bits.resize(n_, 0);
    std::vector<uint8_t> p(m_, 0);
    if (typeA_) {
        auto accumulate = [&](int src, uint8_t v, int row) {
            if (!v) return;
            int m = src % 360;
            for (int x : rows_[row]) p[accAddr(x, m)] ^= 1;
        };
        for (int i = 0; i < k_; i++) accumulate(i, bits[i], i / 360);
        for (int i = 1; i < m1_; i++) p[i] ^= p[i - 1];
        // the dual-diagonal parity bits, interleaved into the code word; they then serve as inputs for the identity part
        for (int s = 0; s < 360; s++)
            for (int t = 0; t < q1_; t++) bits[k_ + 360 * t + s] = p[q1_ * s + t];
        for (int u = 0; u < m1_; u++) {
            int row = k_ / 360 + u / 360;
            if (!bits[k_ + u]) continue;
            int m = u % 360;
            for (int x : rows_[row]) p[accAddr(x, m)] ^= 1;
        }
        for (int s = 0; s < 360; s++)
            for (int t = 0; t < q2_; t++) bits[k_ + m1_ + 360 * t + s] = p[m1_ + q2_ * s + t];
    } else {
        for (int i = 0; i < k_; i++) {
            if (!bits[i]) continue;
            int m = i % 360;
            for (int x : rows_[i / 360]) p[accAddr(x, m)] ^= 1;
        }
        for (int i = 1; i < m_; i++) p[i] ^= p[i - 1];
        for (int i = 0; i < m_; i++) bits[k_ + i] = p[i];
    }
}

void Ldpc::buildGraph() {
    std::vector<std::vector<int>> chk(m_);
    if (typeA_) {
        auto varChain = [&](int i) { return k_ + 360 * (i % q1_) + i / q1_; };
        auto varId = [&](int j) { return k_ + m1_ + 360 * (j % q2_) + j / q2_; };
        for (int b = 0; b < k_ + m1_; b++) {
            int m = b % 360;
            for (int x : rows_[b / 360]) chk[accAddr(x, m)].push_back(b);
        }
        for (int c = 0; c < m1_; c++) {
            chk[c].push_back(varChain(c));
            if (c > 0) chk[c].push_back(varChain(c - 1));
        }
        for (int j = 0; j < m2_; j++) chk[m1_ + j].push_back(varId(j));
    } else {
        for (int b = 0; b < k_; b++) {
            int m = b % 360;
            for (int x : rows_[b / 360]) chk[accAddr(x, m)].push_back(b);
        }
        for (int c = 0; c < m_; c++) {
            chk[c].push_back(k_ + c);
            if (c > 0) chk[c].push_back(k_ + c - 1);
        }
    }
    chkStart_.assign(m_ + 1, 0);
    for (int c = 0; c < m_; c++) chkStart_[c + 1] = chkStart_[c] + (int)chk[c].size();
    chkVar_.resize(chkStart_[m_]);
    for (int c = 0; c < m_; c++) std::copy(chk[c].begin(), chk[c].end(), chkVar_.begin() + chkStart_[c]);
}

bool Ldpc::decode(const std::vector<float>& llr, int maxIter, std::vector<uint8_t>& hard, int* iters, float alpha, bool boxCorr) const {
    const int E = (int)chkVar_.size();
    std::vector<float> msgC2V(E, 0.f), total(llr.begin(), llr.end());
    hard.assign(n_, 0);
    for (int it = 1; it <= maxIter; it++) {
        // check node update (min-sum over the variable-to-check messages total - old check message)
        for (int c = 0; c < m_; c++) {
            int s = chkStart_[c], e = chkStart_[c + 1];
            float min1 = 1e30f, min2 = 1e30f; int minIdx = -1; int neg = 0;
            for (int j = s; j < e; j++) {
                float v = total[chkVar_[j]] - msgC2V[j];
                float a = std::fabs(v);
                if (v < 0) neg ^= 1;
                if (a < min1) { min2 = min1; min1 = a; minIdx = j; }
                else if (a < min2) min2 = a;
            }
            const float c1 = boxCorr ? boxMin1(min1, min2) : min1;
            for (int j = s; j < e; j++) {
                float v = total[chkVar_[j]] - msgC2V[j];
                int sg = neg ^ (v < 0 ? 1 : 0);
                float mag = alpha * (j == minIdx ? min2 : c1);
                float nm = sg ? -mag : mag;
                total[chkVar_[j]] += nm - msgC2V[j];
                msgC2V[j] = nm;
            }
        }
        // hard decision and syndrome
        for (int v = 0; v < n_; v++) hard[v] = total[v] < 0;
        bool ok = true;
        for (int c = 0; c < m_ && ok; c++) {
            int x = 0;
            for (int j = chkStart_[c]; j < chkStart_[c + 1]; j++) x ^= hard[chkVar_[j]];
            if (x) ok = false;
        }
        if (ok) { if (iters) *iters = it; return true; }
    }
    if (iters) *iters = maxIter;
    return false;
}


// ---- layered decoder on 8-bit values, one block per vector lane

namespace {

constexpr int L = Ldpc::kBatch;
constexpr int kMaxCheckDegree = 96;   // Type A 64800 rate 7/15 has checks of degree 86
typedef int8_t V __attribute__((vector_size(16)));   // GCC / Clang vector: SSE on x86-64, NEON on ARM
static_assert(L == 16, "one block per byte of a 16 byte vector");

inline V vsel(V mask, V a, V b) { return (mask & a) | (~mask & b); }
inline V vabsv(V x) { const V s = x >> 7; return (x ^ s) - s; }
// both clamps keep values in +-96 so that adding a message (+-31) never wraps
inline V vclamp(V x, int8_t lim) { const V hi = {lim, lim, lim, lim, lim, lim, lim, lim, lim, lim, lim, lim, lim, lim, lim, lim}; const V lo = -hi; x = vsel(x > hi, hi, x); return vsel(x < lo, lo, x); }

// One iteration over all checks. tot: values of the variables, msg: the messages of the edges.
DECT2_SIMD_BODY void layeredPassBody(const int* chkStart, const int* chkVar, int m, V* tot, V* msg) {
    V v[kMaxCheckDegree];
    const V one = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, big = {127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127}, zero = {};
    for (int c = 0; c < m; c++) {
        const int s = chkStart[c], d = chkStart[c + 1] - s;
        V min1 = big, min2 = big, sg = zero;
        for (int j = 0; j < d; j++) {
            const V x = vclamp(tot[chkVar[s + j]] - msg[s + j], 96);
            v[j] = x;
            const V a = vabsv(x);
            const V lt = a < min1;
            min2 = vsel(lt, min1, vsel(a < min2, a, min2));
            min1 = vsel(lt, a, min1);
            sg ^= (x >> 7);
        }
        const V m1 = vsel(min1 > one, min1 - one, zero), m2 = vsel(min2 > one, min2 - one, zero);   // offset min-sum
        for (int j = 0; j < d; j++) {
            const V x = v[j];
            const V a = vabsv(x);
            V mag = vsel(a == min1, m2, m1);
            const V c31 = {31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31};
            mag = vsel(mag > c31, c31, mag);
            const V neg = sg ^ (x >> 7);   // -1 where the message is negative
            const V out = (mag ^ neg) - neg;
            tot[chkVar[s + j]] = vclamp(x + out, 96);
            msg[s + j] = out;
        }
    }
}
DECT2_AVX2_DISPATCH(void, layeredPass, (const int* chkStart, const int* chkVar, int m, V* tot, V* msg), (chkStart, chkVar, m, tot, msg))

// lanes with a check that fails: bad[l] != 0
DECT2_SIMD_BODY V syndromesBody(const int* chkStart, const int* chkVar, int m, const V* tot) {
    V bad = {};
    for (int c = 0; c < m; c++) {
        V x = {};
        for (int j = chkStart[c]; j < chkStart[c + 1]; j++) x ^= (tot[chkVar[j]] >> 7);
        bad |= x;
    }
    return bad;
}
DECT2_AVX2_DISPATCH(V, syndromes, (const int* chkStart, const int* chkVar, int m, const V* tot), (chkStart, chkVar, m, tot))

} // namespace

void Ldpc::decodeBatch(const std::vector<const int8_t*>& llr, int maxIter, std::vector<std::vector<uint8_t>>& hard, std::vector<char>& ok) const {
    const int nb = (int)llr.size();
    hard.assign((size_t)nb, std::vector<uint8_t>());
    ok.assign((size_t)nb, 0);
    if (nb == 0 || nb > L) {
        DIAG_DEBUG_EVERY(947441, 10.0, "The 8-bit ATSC 3.0 LDPC decoder was given %d code words (1 to %d allowed), so none is decoded in that batch.", nb, (int)L);
        return;
    }
    for (size_t c = 0; c + 1 < chkStart_.size(); c++)
        if (chkStart_[c + 1] - chkStart_[c] > kMaxCheckDegree) {
            DIAG_DEBUG_EVERY(306925, 10.0, "An ATSC 3.0 LDPC check node has more than %d connections, so the 8-bit decoder skips this code.", (int)kMaxCheckDegree);
            return;
        }
    const V init = {31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31};   // unused lanes: the all-zero code word, satisfied from the start
    std::vector<V> tot((size_t)n_, init), msg(chkVar_.size(), V{});
    for (int b = 0; b < nb; b++)
        for (int i = 0; i < n_; i++) tot[(size_t)i][b] = llr[(size_t)b][i];
    V bad = {};
    for (int it = 0; it < maxIter; it++) {
        layeredPass(chkStart_.data(), chkVar_.data(), m_, tot.data(), msg.data());
        bad = syndromes(chkStart_.data(), chkVar_.data(), m_, tot.data());
        bool all = true;
        for (int b = 0; b < nb; b++) if (bad[b]) all = false;
        if (all) break;
    }
    for (int b = 0; b < nb; b++) {
        ok[(size_t)b] = !bad[b];
        hard[(size_t)b].resize((size_t)n_);
        for (int i = 0; i < n_; i++) hard[(size_t)b][(size_t)i] = tot[(size_t)i][b] < 0;
    }
}

const Ldpc& ldpcCode(int n, int rate15) {
    static std::mutex mu;
    static std::map<int, std::unique_ptr<Ldpc>> cache;
    std::lock_guard<std::mutex> lk(mu);
    auto& p = cache[n * 100 + rate15];
    if (!p) p.reset(new Ldpc(rate15, n));
    return *p;
}

const Ldpc& ldpc16200(int rate15) { return ldpcCode(16200, rate15); }

const dect2::LdpcCode* qcCode(int n, int rate15) {
    static std::mutex mu;
    static std::map<int, std::unique_ptr<dect2::LdpcCode>> cache;
    const Ldpc& a = ldpcCode(n, rate15);
    if (!a.ok() || a.typeA()) return nullptr;
    std::lock_guard<std::mutex> lk(mu);
    auto& p = cache[n * 100 + rate15];
    if (!p) {
        const int* lens; int nrows;
        const int* tab = n == 16200 ? ldpcTable16200(rate15, &lens, &nrows) : ldpcTable64800(rate15, &lens, &nrows);
        std::vector<std::vector<int>> rows;
        int pos = 0;
        for (int r = 0; r < nrows; r++) { rows.emplace_back(tab + pos, tab + pos + lens[r]); pos += lens[r]; }
        p.reset(new dect2::LdpcCode(a.k(), n, rows));
    }
    return p.get();
}

// Measured on an ideal channel (BPSK per dimension, the noisy words the same for every factor), 0.76 .. 0.88 tried: 0.80 to 0.84 is best for every
// Type B code (16200 rate 8/15 at 1.4 dB: 2 failed words of 40 at 0.80 and 0.84, 25 at 0.76, 9 at 0.88; the receiver's old 8-bit offset
// min-sum with the float fallback: 37).
float qcAlpha(int n, int rate15) {
    (void)n; (void)rate15;
    return 0.82f;
}

// Type A codes on the general decoder with the correction: 0.80 (64800 rates 2/15..7/15 and 16200 3/15..5/15), 0.92 for 16200 rate 2/15. The
// plain normalised min-sum at 0.8 needed 0.2 to 0.4 dB more (64800 rate 3/15 at -4.26 dB: 20 failed words of 20 against 1 of 20).
float typeAAlpha(int n, int rate15) { return n == 16200 && rate15 == 2 ? 0.92f : 0.80f; }

} // namespace atsc3
} // namespace dect2
