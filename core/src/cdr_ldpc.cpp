// CDR LDPC codes (see cdr_ldpc.h). The encoder inverts the parity part of H in the ring of 256 x 256 circulants (polynomials modulo
// x^256 + 1): that ring is local, so the matrix is invertible exactly when every pivot can be chosen with an odd number of terms.
#include "dect2/cdr_ldpc.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace dect2::cdr {

int ldpcInfoBits(int rate) {
    static const int k[4] = {2304, 3072, 4608, 6912};
    return rate >= 0 && rate < 4 ? k[rate] : 0;
}
const char* ldpcRateText(int rate) {
    static const char* t[4] = {"1/4", "1/3", "1/2", "3/4"};
    return rate >= 0 && rate < 4 ? t[rate] : "-";
}

namespace {

// a polynomial modulo x^256 + 1 over GF(2): bit t of the 256 bits is the coefficient of x^t
struct Poly {
    std::array<uint64_t, 4> w{};
    bool zero() const { return !(w[0] | w[1] | w[2] | w[3]); }
    bool odd() const { int c = 0; for (uint64_t v : w) c += __builtin_popcountll(v); return c & 1; }
    bool bit(int t) const { return (w[(size_t)(t >> 6)] >> (t & 63)) & 1u; }
    void flip(int t) { w[(size_t)(t >> 6)] ^= (uint64_t)1 << (t & 63); }
};

Poly monomial(int t) { Poly p; p.flip(((t % 256) + 256) % 256); return p; }

// p * x^t
Poly rotl(const Poly& p, int t) {
    t &= 255;
    if (!t) return p;
    const int ws = t >> 6, bs = t & 63;
    Poly r;
    for (int i = 0; i < 4; i++) {
        const uint64_t v = p.w[(size_t)i];
        const int d = (i + ws) & 3;
        r.w[(size_t)d] ^= bs ? v << bs : v;
        if (bs) r.w[(size_t)((d + 1) & 3)] ^= v >> (64 - bs);
    }
    return r;
}

void addTo(Poly& a, const Poly& b) { for (int i = 0; i < 4; i++) a.w[(size_t)i] ^= b.w[(size_t)i]; }

Poly mul(const Poly& a, const Poly& b) {
    Poly r;
    for (int i = 0; i < 4; i++) {
        uint64_t v = a.w[(size_t)i];
        while (v) {
            const int t = __builtin_ctzll(v);
            v &= v - 1;
            addTo(r, rotl(b, i * 64 + t));
        }
    }
    return r;
}

Poly square(const Poly& a) {
    Poly r;
    for (int t = 0; t < 256; t++) if (a.bit(t)) r.flip((2 * t) & 255);
    return r;
}

// the units of the ring form a group of order 2^255: u^-1 = u^(2^255 - 1) = product of u^(2^i), i = 0 .. 254
Poly inverse(const Poly& u) {
    Poly r = monomial(0), s = u;
    for (int i = 0; i < 255; i++) { r = mul(r, s); s = square(s); }
    return r;
}

} // namespace

CdrLdpc::CdrLdpc(int rate) : rate_(rate), k_(ldpcInfoBits(rate)), m_(kLdpcBits - ldpcInfoBits(rate)) {
    int nb = 0, nh = 0;
    const LdpcBlock* b = ldpcBlocks(rate, nb);
    const LdpcHole* h = ldpcHoles(rate, nh);
    std::vector<std::vector<int>> rows((size_t)m_);
    for (int e = 0; e < nb; e++) {
        for (int i = 0; i < kLdpcZ; i++) {
            bool hole = false;
            for (int q = 0; q < nh; q++)
                if (h[q].row == b[e].row && h[q].col == b[e].col && h[q].shift == b[e].shift && i >= h[q].first && i < h[q].first + h[q].count) hole = true;
            if (hole) continue;
            rows[(size_t)(b[e].row * kLdpcZ + i)].push_back(b[e].col * kLdpcZ + (i + b[e].shift) % kLdpcZ);
        }
    }
    rowStart_.assign((size_t)m_ + 1, 0);
    for (int r = 0; r < m_; r++) {
        std::sort(rows[(size_t)r].begin(), rows[(size_t)r].end());
        rowStart_[(size_t)r + 1] = rowStart_[(size_t)r] + (int)rows[(size_t)r].size();
        rowVar_.insert(rowVar_.end(), rows[(size_t)r].begin(), rows[(size_t)r].end());
    }
    // information variable -> checks
    std::vector<std::vector<int>> cols((size_t)k_);
    for (int r = 0; r < m_; r++)
        for (int e = rowStart_[(size_t)r]; e < rowStart_[(size_t)r + 1]; e++)
            if (rowVar_[(size_t)e] < k_) cols[(size_t)rowVar_[(size_t)e]].push_back(r);
    colStart_.assign((size_t)k_ + 1, 0);
    for (int v = 0; v < k_; v++) {
        colStart_[(size_t)v + 1] = colStart_[(size_t)v] + (int)cols[(size_t)v].size();
        colChk_.insert(colChk_.end(), cols[(size_t)v].begin(), cols[(size_t)v].end());
    }
    buildEncoder();
}

std::vector<int> CdrLdpc::row(int r) const {
    if (r < 0 || r >= m_) return {};
    return std::vector<int>(rowVar_.begin() + rowStart_[(size_t)r], rowVar_.begin() + rowStart_[(size_t)r + 1]);
}

void CdrLdpc::buildEncoder() {
    const int mb = m_ / kLdpcZ, kb = k_ / kLdpcZ;
    int nb = 0;
    const LdpcBlock* b = ldpcBlocks(rate_, nb);
    // A (parity part) as polynomials: a circulant with shift s maps the vector v(x) to x^-s v(x)
    std::vector<std::vector<Poly>> a((size_t)mb, std::vector<Poly>((size_t)(2 * mb)));
    for (int e = 0; e < nb; e++)
        if (b[e].col >= kb) addTo(a[b[e].row][(size_t)(b[e].col - kb)], monomial(kLdpcZ - b[e].shift));
    for (int i = 0; i < mb; i++) a[(size_t)i][(size_t)(mb + i)] = monomial(0);
    // Gauss-Jordan with unit pivots
    for (int c = 0; c < mb; c++) {
        int p = -1;
        for (int r = c; r < mb; r++) if (a[(size_t)r][(size_t)c].odd()) { p = r; break; }
        if (p < 0) { encOk_ = false; return; }
        std::swap(a[(size_t)c], a[(size_t)p]);
        const Poly inv = inverse(a[(size_t)c][(size_t)c]);
        for (auto& x : a[(size_t)c]) if (!x.zero()) x = mul(inv, x);
        for (int r = 0; r < mb; r++) {
            if (r == c || a[(size_t)r][(size_t)c].zero()) continue;
            const Poly f = a[(size_t)r][(size_t)c];
            for (int j = 0; j < 2 * mb; j++) if (!a[(size_t)c][(size_t)j].zero()) addTo(a[(size_t)r][(size_t)j], mul(f, a[(size_t)c][(size_t)j]));
        }
    }
    // parity bits of every single information bit: P = A^-1 S, where S has one term per check the bit takes part in
    genWords_ = m_ / 64;
    gen_.assign((size_t)k_ * (size_t)genWords_, 0);
    for (int v = 0; v < k_; v++) {
        uint64_t* g = &gen_[(size_t)v * (size_t)genWords_];
        for (int i = 0; i < mb; i++) {
            Poly acc;
            for (int e = colStart_[(size_t)v]; e < colStart_[(size_t)v + 1]; e++) {
                const int chk = colChk_[(size_t)e];
                addTo(acc, rotl(a[(size_t)i][(size_t)(mb + chk / kLdpcZ)], chk % kLdpcZ));
            }
            for (int q = 0; q < 4; q++) g[(size_t)(i * 4 + q)] = acc.w[(size_t)q];
        }
    }
    encOk_ = true;
}

void CdrLdpc::encode(const uint8_t* info, uint8_t* word) const {
    std::vector<uint64_t> par((size_t)genWords_, 0);
    for (int v = 0; v < k_; v++) {
        word[v] = info[v] & 1;
        if (!word[v]) continue;
        const uint64_t* g = &gen_[(size_t)v * (size_t)genWords_];
        for (int q = 0; q < genWords_; q++) par[(size_t)q] ^= g[q];
    }
    for (int t = 0; t < m_; t++) word[k_ + t] = (uint8_t)((par[(size_t)(t >> 6)] >> (t & 63)) & 1u);
}

int CdrLdpc::syndromeWeight(const uint8_t* word) const {
    int w = 0;
    for (int r = 0; r < m_; r++) {
        int s = 0;
        for (int e = rowStart_[(size_t)r]; e < rowStart_[(size_t)r + 1]; e++) s ^= word[rowVar_[(size_t)e]] & 1;
        w += s;
    }
    return w;
}

CdrLdpc::Result CdrLdpc::decode(const float* llr, uint8_t* info, int maxIter) const {
    static thread_local std::vector<float> L, R;
    L.assign(llr, llr + kLdpcBits);
    R.assign(rowVar_.size(), 0.f);
    static const float kAlpha[4] = {0.9f, 0.85f, 0.8f, 0.75f};   // the low rates have checks of degree 4 to 6: less damping (measured on AWGN)
    const float alpha = kAlpha[rate_];
    auto satisfied = [&]() {
        for (int r = 0; r < m_; r++) {
            int s = 0;
            for (int e = rowStart_[(size_t)r]; e < rowStart_[(size_t)r + 1]; e++) s ^= L[(size_t)rowVar_[(size_t)e]] < 0.f;
            if (s) return false;
        }
        return true;
    };
    Result res;
    if (satisfied()) res.ok = true;
    float q[32];
    for (int it = 0; it < maxIter && !res.ok; it++) {
        for (int r = 0; r < m_; r++) {
            const int e0 = rowStart_[(size_t)r], e1 = rowStart_[(size_t)r + 1], d = e1 - e0;
            float m1 = 1e30f, m2 = 1e30f;
            int pos = -1;
            unsigned sgn = 0;
            for (int i = 0; i < d; i++) {
                const float v = L[(size_t)rowVar_[(size_t)(e0 + i)]] - R[(size_t)(e0 + i)];
                q[i] = v;
                const float av = std::fabs(v);
                sgn ^= v < 0.f;
                if (av < m1) { m2 = m1; m1 = av; pos = i; } else if (av < m2) m2 = av;
            }
            for (int i = 0; i < d; i++) {
                const float mag = alpha * (i == pos ? m2 : m1);
                const bool neg = (sgn ^ (q[i] < 0.f)) & 1u;
                const float nr = neg ? -mag : mag;
                R[(size_t)(e0 + i)] = nr;
                L[(size_t)rowVar_[(size_t)(e0 + i)]] = q[i] + nr;
            }
        }
        res.iterations = it + 1;
        if (satisfied()) res.ok = true;
    }
    for (int v = 0; v < k_; v++) info[v] = L[(size_t)v] < 0.f;
    return res;
}

const CdrLdpc& cdrLdpc(int rate) {
    static std::once_flag once[4];
    static std::unique_ptr<CdrLdpc> codes[4];
    const int r = std::max(0, std::min(3, rate));
    std::call_once(once[r], [r] { codes[r] = std::make_unique<CdrLdpc>(r); });
    return *codes[r];
}

} // namespace dect2::cdr
