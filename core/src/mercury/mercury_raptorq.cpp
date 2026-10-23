// RaptorQ, RFC 6330 (see mercury_raptorq.h).
#include "dect2/mercury/mercury_raptorq.h"
#include <algorithm>
#include <cstring>

namespace dect2 {
namespace mercury {

namespace {
#include "mercury_raptorq_tables.inc"

// the octet field of RFC 6330 5.7 (x^8 + x^4 + x^3 + x^2 + 1), alpha = 2
struct Oct {
    uint8_t exp[512], log[256];
    Oct() {
        int x = 1;
        for (int i = 0; i < 255; i++) { exp[i] = (uint8_t)x; log[x] = (uint8_t)i; x <<= 1; if (x & 0x100) x ^= 0x11D; }
        for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
        log[0] = 0;
    }
    uint8_t mul(uint8_t a, uint8_t b) const { return (a && b) ? exp[log[a] + log[b]] : 0; }
    uint8_t div(uint8_t a, uint8_t b) const { return a ? exp[log[a] + 255 - log[b]] : 0; }
};
const Oct& oct() { static const Oct o; return o; }

// dst ^= c * src
void axpy(uint8_t* dst, const uint8_t* src, uint8_t c, size_t n) {
    if (!c) return;
    if (c == 1) { for (size_t k = 0; k < n; k++) dst[k] ^= src[k]; return; }
    const Oct& o = oct();
    const int lc = o.log[c];
    for (size_t k = 0; k < n; k++) if (src[k]) dst[k] ^= o.exp[lc + o.log[src[k]]];
}

uint32_t rqRand(uint32_t y, uint32_t i, uint32_t m) {
    const uint32_t x0 = (y + i) & 0xFF, x1 = ((y >> 8) + i) & 0xFF, x2 = ((y >> 16) + i) & 0xFF, x3 = ((y >> 24) + i) & 0xFF;
    return (kRqV0[x0] ^ kRqV1[x1] ^ kRqV2[x2] ^ kRqV3[x3]) % m;
}

const uint32_t kDegree[31] = {0, 5243, 529531, 704294, 791675, 844104, 879057, 904023, 922747, 937311, 948962, 958494, 966438, 973160,
                              978921, 983914, 988283, 992138, 995565, 998631, 1001391, 1003887, 1006157, 1008229, 1010129, 1011876,
                              1013490, 1014983, 1016370, 1017662, 1048576};

uint32_t deg(uint32_t v, uint32_t W) {
    for (uint32_t d = 0; d < 31; d++) if (v < kDegree[d]) return std::min(d, W - 2);
    return 0;
}

bool isPrime(uint32_t n) {
    if (n < 2) return false;
    for (uint32_t i = 2; (uint64_t)i * i <= n; i++) if (n % i == 0) return false;
    return true;
}

// the columns of the LT row of internal symbol index X (RFC 5.3.5.3 Enc), duplicates cancelled
std::vector<uint32_t> ltCols(const RqParams& p, uint32_t X) {
    const uint32_t A = (53591 + p.J * 997) | 1, Bt = 10267 * (p.J + 1);
    const uint32_t y = Bt + X * A;
    const uint32_t v = rqRand(y, 0, 1u << 20);
    const uint32_t d = deg(v, p.W);
    const uint32_t a = 1 + rqRand(y, 1, p.W - 1);
    uint32_t b = rqRand(y, 2, p.W);
    const uint32_t d1 = d < 4 ? 2 + rqRand(X, 3, 2) : 2;
    const uint32_t a1 = 1 + rqRand(X, 4, p.P1 - 1);
    uint32_t b1 = rqRand(X, 5, p.P1);
    std::vector<uint32_t> c;
    c.push_back(b);
    for (uint32_t j = 1; j < d; j++) { b = (b + a) % p.W; c.push_back(b); }
    while (b1 >= p.P) b1 = (b1 + a1) % p.P1;
    c.push_back(p.W + b1);
    for (uint32_t j = 1; j < d1; j++) {
        b1 = (b1 + a1) % p.P1;
        while (b1 >= p.P) b1 = (b1 + a1) % p.P1;
        c.push_back(p.W + b1);
    }
    std::sort(c.begin(), c.end());
    std::vector<uint32_t> u;
    for (size_t i = 0; i < c.size();) {
        size_t j = i;
        while (j < c.size() && c[j] == c[i]) j++;
        if ((j - i) & 1) u.push_back(c[i]);
        i = j;
    }
    return u;
}

} // namespace

RqParams rqParams(uint32_t K) {
    RqParams p;
    p.K = K;
    const RqSystematic* e = &kRqTable[476];
    for (const RqSystematic& r : kRqTable) if (K <= r.kp) { e = &r; break; }
    p.Kp = e->kp; p.J = e->j; p.S = e->s; p.H = e->h; p.W = e->w;
    p.L = p.Kp + p.S + p.H;
    p.P = p.L - p.W;
    p.B = p.W - p.S;
    p.P1 = p.P;
    while (!isPrime(p.P1)) p.P1++;
    return p;
}

uint32_t rqBlockSymbols(uint32_t F, uint32_t T, uint32_t Z, uint32_t sbn) {
    if (!T || !Z) return 0;
    const uint32_t Kt = (uint32_t)(((uint64_t)F + T - 1) / T);
    const uint32_t KL = (Kt + Z - 1) / Z, KS = Kt / Z;
    const uint32_t ZL = Kt - KS * Z;
    return sbn < ZL ? KL : KS;
}

bool rqSolve(const RqParams& p, uint32_t T, const std::vector<uint32_t>& isi, const std::vector<const uint8_t*>& sym,
             std::vector<std::vector<uint8_t>>& C) {
    const uint32_t L = p.L, W = p.W, S = p.S, H = p.H, KS = p.Kp + p.S;
    const Oct& o = oct();
    // binary rows: the LDPC rows (symbol 0), then the LT rows of the received symbols
    std::vector<std::vector<uint32_t>> rows;
    std::vector<const uint8_t*> rsym;
    rows.reserve(S + isi.size());
    {
        std::vector<std::vector<uint32_t>> ld(S);
        for (uint32_t i = 0; i < p.B; i++) {
            const uint32_t a = 1 + i / S;
            uint32_t b = i % S;
            ld[b].push_back(i); b = (b + a) % S;
            ld[b].push_back(i); b = (b + a) % S;
            ld[b].push_back(i);
        }
        for (uint32_t i = 0; i < S; i++) {
            ld[i].push_back(p.B + i);
            ld[i].push_back(W + i % p.P);
            ld[i].push_back(W + (i + 1) % p.P);
        }
        for (auto& r : ld) {
            std::sort(r.begin(), r.end());
            std::vector<uint32_t> u;
            for (size_t i = 0; i < r.size();) { size_t j = i; while (j < r.size() && r[j] == r[i]) j++; if ((j - i) & 1) u.push_back(r[i]); i = j; }
            rows.push_back(std::move(u));
            rsym.push_back(nullptr);
        }
    }
    for (size_t i = 0; i < isi.size(); i++) { rows.push_back(ltCols(p, isi[i])); rsym.push_back(sym[i]); }
    const size_t R = rows.size();
    // HDPC rows (RFC 5.3.3.3: MT x GAMMA, then the identity), computed by the recursion over the columns from the right
    std::vector<std::vector<uint8_t>> hd(H, std::vector<uint8_t>(KS, 0));
    for (uint32_t r = 0; r < H; r++) hd[r][KS - 1] = o.exp[r];
    for (int64_t col = (int64_t)KS - 2; col >= 0; col--) {
        for (uint32_t r = 0; r < H; r++) hd[r][(size_t)col] = hd[r][(size_t)col + 1] ? o.exp[o.log[hd[r][(size_t)col + 1]] + 1] : 0;
        const uint32_t b1 = rqRand((uint32_t)col + 1, 6, H);
        const uint32_t b2 = (b1 + rqRand((uint32_t)col + 1, 7, H - 1) + 1) % H;
        hd[b1][(size_t)col] ^= 1;
        hd[b2][(size_t)col] ^= 1;
    }
    // peeling over the active columns 0 .. W - 1 with inactivation; W .. L - 1 are inactive from the start
    std::vector<std::vector<uint32_t>> colRows(W);
    std::vector<int> cnt(R, 0);
    for (size_t r = 0; r < R; r++)
        for (uint32_t c : rows[r]) if (c < W) { colRows[c].push_back((uint32_t)r); cnt[r]++; }
    std::vector<int> state(L, 0);           // 0 open, 1 resolved, 2 inactive
    std::vector<int> pivotRow(L, -1), inactIdx(L, -1);
    std::vector<uint32_t> inactive;
    for (uint32_t c = W; c < L; c++) { state[c] = 2; inactIdx[c] = (int)inactive.size(); inactive.push_back(c); }
    std::vector<uint8_t> used(R, 0);
    std::vector<uint32_t> order;            // resolved columns, in order
    std::vector<uint32_t> queue;
    for (size_t r = 0; r < R; r++) if (cnt[r] == 1) queue.push_back((uint32_t)r);
    uint32_t open = W;
    auto close = [&](uint32_t c) {
        for (uint32_t r2 : colRows[c]) { if (--cnt[r2] == 1 && !used[r2]) queue.push_back(r2); }
        open--;
    };
    while (open > 0) {
        bool progressed = false;
        while (!queue.empty()) {
            const uint32_t r = queue.back();
            queue.pop_back();
            if (used[r] || cnt[r] != 1) continue;
            uint32_t c = UINT32_MAX;
            for (uint32_t x : rows[r]) if (x < W && state[x] == 0) { c = x; break; }
            if (c == UINT32_MAX) continue;
            used[r] = 1;
            state[c] = 1; pivotRow[c] = (int)r;
            order.push_back(c);
            close(c);
            progressed = true;
        }
        if (open == 0) break;
        // stuck: the open row with the fewest open columns gives up all but one of them to the inactive set
        int best = -1;
        for (size_t r = 0; r < R; r++) if (!used[r] && cnt[r] >= 2 && (best < 0 || cnt[r] < cnt[(size_t)best])) best = (int)r;
        if (best < 0) {
            // no row reaches the open columns any more: inactivate them (the dense stage may still solve them)
            for (uint32_t c = 0; c < W; c++) if (state[c] == 0) { state[c] = 2; inactIdx[c] = (int)inactive.size(); inactive.push_back(c); close(c); }
            break;
        }
        bool kept = false;
        for (uint32_t x : rows[(size_t)best]) {
            if (x >= W || state[x] != 0) continue;
            if (!kept) { kept = true; continue; }
            state[x] = 2; inactIdx[x] = (int)inactive.size(); inactive.push_back(x);
            close(x);
        }
        (void)progressed;
    }
    const size_t NI = inactive.size(), words = (NI + 63) / 64;
    // the value of every resolved column: a symbol and a set of inactive columns (GF(2): all peeled rows are binary)
    std::vector<std::vector<uint8_t>> val(L);
    std::vector<std::vector<uint64_t>> dep(L);
    for (uint32_t c : order) {
        const uint32_t r = (uint32_t)pivotRow[c];
        std::vector<uint8_t> v(T, 0);
        std::vector<uint64_t> d(words, 0);
        if (rsym[r]) std::memcpy(v.data(), rsym[r], T);
        for (uint32_t x : rows[r]) {
            if (x == c) continue;
            if (state[x] == 2) d[(size_t)inactIdx[x] >> 6] ^= 1ull << (inactIdx[x] & 63);
            else { for (uint32_t k = 0; k < T; k++) v[k] ^= val[x][k]; for (size_t w = 0; w < words; w++) d[w] ^= dep[x][w]; }
        }
        val[c] = std::move(v); dep[c] = std::move(d);
    }
    // the dense system in the inactive columns: the binary rows that were not pivots, and the HDPC rows
    std::vector<std::vector<uint8_t>> A, rhs;
    for (size_t r = 0; r < R; r++) {
        if (used[r]) continue;
        std::vector<uint8_t> a(NI, 0), b(T, 0);
        if (rsym[r]) std::memcpy(b.data(), rsym[r], T);
        for (uint32_t x : rows[r]) {
            if (state[x] == 2) a[(size_t)inactIdx[x]] ^= 1;
            else if (state[x] == 1) {
                for (uint32_t k = 0; k < T; k++) b[k] ^= val[x][k];
                for (size_t i = 0; i < NI; i++) if ((dep[x][i >> 6] >> (i & 63)) & 1) a[i] ^= 1;
            }
        }
        A.push_back(std::move(a)); rhs.push_back(std::move(b));
    }
    for (uint32_t h = 0; h < H; h++) {
        std::vector<uint8_t> a(NI, 0), b(T, 0);
        for (uint32_t c = 0; c < KS; c++) {
            const uint8_t g = hd[h][c];
            if (!g) continue;
            if (state[c] == 2) a[(size_t)inactIdx[c]] ^= g;
            else if (state[c] == 1) {
                axpy(b.data(), val[c].data(), g, T);
                for (size_t w = 0; w < words; w++) {
                    uint64_t m = dep[c][w];
                    while (m) { const int bit = __builtin_ctzll(m); m &= m - 1; a[(w << 6) + (size_t)bit] ^= g; }
                }
            }
        }
        a[(size_t)inactIdx[KS + h]] ^= 1;   // the identity part of G_HDPC
        A.push_back(std::move(a)); rhs.push_back(std::move(b));
    }
    // Gaussian elimination over GF(256)
    const size_t NE = A.size();
    if (NE < NI) return false;
    std::vector<size_t> pr(NI);
    size_t rowAt = 0;
    for (size_t col = 0; col < NI; col++) {
        size_t piv = NE;
        for (size_t r = rowAt; r < NE; r++) if (A[r][col]) { piv = r; break; }
        if (piv == NE) return false;
        std::swap(A[rowAt], A[piv]); std::swap(rhs[rowAt], rhs[piv]);
        const uint8_t inv = o.div(1, A[rowAt][col]);
        if (inv != 1) {
            for (size_t i = col; i < NI; i++) A[rowAt][i] = o.mul(A[rowAt][i], inv);
            for (uint32_t k = 0; k < T; k++) rhs[rowAt][k] = o.mul(rhs[rowAt][k], inv);
        }
        for (size_t r = 0; r < NE; r++) {
            if (r == rowAt || !A[r][col]) continue;
            const uint8_t f = A[r][col];
            axpy(A[r].data() + col, A[rowAt].data() + col, f, NI - col);
            axpy(rhs[r].data(), rhs[rowAt].data(), f, T);
        }
        pr[col] = rowAt;
        rowAt++;
    }
    C.assign(L, std::vector<uint8_t>(T, 0));
    for (size_t i = 0; i < NI; i++) C[inactive[i]] = rhs[pr[i]];
    for (uint32_t c : order) {
        std::vector<uint8_t> v = val[c];
        for (size_t w = 0; w < words; w++) {
            uint64_t m = dep[c][w];
            while (m) { const int bit = __builtin_ctzll(m); m &= m - 1; const std::vector<uint8_t>& s = C[inactive[(w << 6) + (size_t)bit]]; for (uint32_t k = 0; k < T; k++) v[k] ^= s[k]; }
        }
        C[c] = std::move(v);
    }
    return true;
}

std::vector<uint8_t> rqEnc(const RqParams& p, uint32_t T, const std::vector<std::vector<uint8_t>>& C, uint32_t isi) {
    std::vector<uint8_t> s(T, 0);
    for (uint32_t c : ltCols(p, isi)) for (uint32_t k = 0; k < T; k++) s[k] ^= C[c][k];
    return s;
}

RqBlockDecoder::RqBlockDecoder(uint32_t K, uint32_t T) : K_(K), T_(T) {}

bool RqBlockDecoder::add(uint32_t esi, const uint8_t* sym) {
    if (done_) return false;
    for (uint32_t e : esis_) if (e == esi) return false;
    esis_.push_back(esi);
    syms_.emplace_back(sym, sym + T_);
    return true;
}

bool RqBlockDecoder::tryDecode() {
    if (done_) return true;
    const uint32_t n = (uint32_t)esis_.size();
    if (n < K_ || K_ == 0) return false;
    if (triedAt_ && n < triedAt_ + std::max<uint32_t>(1, K_ / 50)) return false;
    triedAt_ = n;
    const RqParams p = rqParams(K_);
    std::vector<uint32_t> isi;
    std::vector<const uint8_t*> sym;
    std::vector<uint8_t> zero(T_, 0);
    std::vector<int> have(K_, -1);
    for (uint32_t i = 0; i < n; i++) {
        isi.push_back(esis_[i] < K_ ? esis_[i] : esis_[i] + (p.Kp - K_));
        sym.push_back(syms_[i].data());
        if (esis_[i] < K_) have[esis_[i]] = (int)i;
    }
    for (uint32_t i = K_; i < p.Kp; i++) { isi.push_back(i); sym.push_back(zero.data()); }
    // all source symbols there: nothing to solve
    bool all = true;
    for (uint32_t i = 0; i < K_; i++) all &= have[i] >= 0;
    data_.assign((size_t)K_ * T_, 0);
    if (all) {
        for (uint32_t i = 0; i < K_; i++) std::memcpy(&data_[(size_t)i * T_], syms_[(size_t)have[i]].data(), T_);
        done_ = true;
        return true;
    }
    std::vector<std::vector<uint8_t>> C;
    if (!rqSolve(p, T_, isi, sym, C)) return false;
    for (uint32_t i = 0; i < K_; i++) {
        if (have[i] >= 0) std::memcpy(&data_[(size_t)i * T_], syms_[(size_t)have[i]].data(), T_);
        else { const std::vector<uint8_t> s = rqEnc(p, T_, C, i); std::memcpy(&data_[(size_t)i * T_], s.data(), T_); }
    }
    done_ = true;
    return true;
}

RqBlockEncoder::RqBlockEncoder(uint32_t K, uint32_t T, const uint8_t* source) : p_(rqParams(K)), T_(T) {
    std::vector<uint32_t> isi;
    std::vector<const uint8_t*> sym;
    std::vector<uint8_t> zero(T, 0);
    for (uint32_t i = 0; i < p_.Kp; i++) { isi.push_back(i); sym.push_back(i < K ? source + (size_t)i * T : zero.data()); }
    ok_ = rqSolve(p_, T, isi, sym, C_);
}

std::vector<uint8_t> RqBlockEncoder::symbol(uint32_t esi) const {
    const uint32_t isi = esi < p_.K ? esi : esi + (p_.Kp - p_.K);
    return rqEnc(p_, T_, C_, isi);
}

} // namespace mercury
} // namespace dect2
