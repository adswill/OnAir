#include "atsc3_ldpc.h"
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
    if (rate15 < 2 || rate15 > 13 || (n != 16200 && n != 64800)) return;
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

bool Ldpc::decode(const std::vector<float>& llr, int maxIter, std::vector<uint8_t>& hard, int* iters) const {
    const int E = (int)chkVar_.size();
    std::vector<float> msgC2V(E, 0.f), total(llr.begin(), llr.end());
    hard.assign(n_, 0);
    const float alpha = 0.8f;
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
            for (int j = s; j < e; j++) {
                float v = total[chkVar_[j]] - msgC2V[j];
                int sg = neg ^ (v < 0 ? 1 : 0);
                float mag = alpha * (j == minIdx ? min2 : min1);
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

const Ldpc& ldpcCode(int n, int rate15) {
    static std::mutex mu;
    static std::map<int, std::unique_ptr<Ldpc>> cache;
    std::lock_guard<std::mutex> lk(mu);
    auto& p = cache[n * 100 + rate15];
    if (!p) p.reset(new Ldpc(rate15, n));
    return *p;
}

const Ldpc& ldpc16200(int rate15) { return ldpcCode(16200, rate15); }

} // namespace atsc3
} // namespace dect2
