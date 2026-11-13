// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode Builder FEC: repetition, positional Hamming (and the extended codes), convolutional codes with a hard-decision Viterbi decoder,
// and Reed-Solomon over GF(2^m) for any field polynomial, first root and root spacing (Berlekamp-Massey, Chien, Forney).
#include "dect2/modebuilder/modebuilder.h"
#include <algorithm>
#include <numeric>

namespace dect2::modebuilder {

// ---------------------------------------------------------------- Reed-Solomon
ReedSolomon::ReedSolomon(int m, uint32_t poly, int n, int k, int fcr, int prim) : n_(n), k_(k), fcr_(fcr), prim_(prim), nn_((1 << m) - 1) {
    if (m < 2 || m > 12 || n > nn_ || k < 1 || k >= n || prim < 1) return;
    if (std::gcd(prim, nn_) != 1) return;
    exp_.assign((size_t)nn_ * 2 + 2, 0);
    log_.assign((size_t)nn_ + 1, -1);
    uint32_t x = 1;
    for (int i = 0; i < nn_; i++) {
        if (log_[x] >= 0) return;   // repeats: the polynomial is not primitive
        exp_[(size_t)i] = (int)x;
        log_[x] = i;
        x <<= 1;
        if (x & (1u << m)) x ^= poly;
        x &= (1u << m) - 1;
        if (x == 0) return;
    }
    if (x != 1) return;
    for (int i = nn_; i < (int)exp_.size(); i++) exp_[(size_t)i] = exp_[(size_t)(i - nn_)];
    // g(x) = prod (x - b^(fcr + i)), b = a^prim; gen_[i] is the coefficient of x^i
    const int nk = n - k;
    gen_.assign((size_t)nk + 1, 0);
    gen_[0] = 1;
    for (int i = 0; i < nk; i++) {
        const int root = exp_[(size_t)(((long long)(fcr + i) * prim) % nn_)];
        for (int j = i + 1; j > 0; j--) gen_[(size_t)j] = gen_[(size_t)(j - 1)] ^ mul(gen_[(size_t)j], root);
        gen_[0] = mul(gen_[0], root);
    }
    ok_ = true;
}

int ReedSolomon::mul(int a, int b) const { return a && b ? exp_[(size_t)(log_[(size_t)a] + log_[(size_t)b])] : 0; }

std::vector<int> ReedSolomon::encode(const std::vector<int>& data) const {
    const int nk = n_ - k_;
    std::vector<int> r((size_t)nk, 0);
    for (int dv : data) {
        const int fb = (dv & nn_) ^ r[0];
        for (int j = 0; j + 1 < nk; j++) r[(size_t)j] = r[(size_t)(j + 1)] ^ mul(fb, gen_[(size_t)(nk - 1 - j)]);
        r[(size_t)(nk - 1)] = mul(fb, gen_[0]);
    }
    std::vector<int> cw = data;
    cw.insert(cw.end(), r.begin(), r.end());
    return cw;
}

int ReedSolomon::decode(std::vector<int>& cw) const {
    if (!ok_) return -1;
    const int nk = n_ - k_;
    const int L = (int)cw.size();
    if (L <= nk || L > n_) return -1;
    auto pw = [&](long long e) { e %= nn_; if (e < 0) e += nn_; return exp_[(size_t)e]; };
    std::vector<int> S((size_t)nk);
    bool any = false;
    for (int j = 0; j < nk; j++) {
        const int root = pw((long long)(fcr_ + j) * prim_);
        int s = 0;
        for (int c : cw) s = mul(s, root) ^ (c & nn_);
        S[(size_t)j] = s;
        any |= s != 0;
    }
    if (!any) return 0;
    // Berlekamp-Massey
    std::vector<int> lam{1}, B{1};
    int Ldeg = 0, mm = 1, b = 1;
    for (int r = 0; r < nk; r++) {
        int d = S[(size_t)r];
        for (int i = 1; i <= Ldeg && i < (int)lam.size(); i++) d ^= mul(lam[(size_t)i], S[(size_t)(r - i)]);
        if (d == 0) { mm++; continue; }
        const int coef = mul(d, exp_[(size_t)((nn_ - log_[(size_t)b]) % nn_)]);
        std::vector<int> T = lam;
        if (lam.size() < B.size() + (size_t)mm) lam.resize(B.size() + (size_t)mm, 0);
        for (size_t i = 0; i < B.size(); i++) lam[i + (size_t)mm] ^= mul(coef, B[i]);
        if (2 * Ldeg <= r) { Ldeg = r + 1 - Ldeg; B = T; b = d; mm = 1; }
        else mm++;
    }
    while (lam.size() > 1 && lam.back() == 0) lam.pop_back();
    const int nerr = (int)lam.size() - 1;
    if (nerr != Ldeg || nerr == 0 || 2 * nerr > nk) return -1;
    // Chien search over the positions that exist (a shortened code word has fewer)
    std::vector<int> where;   // indices into cw
    for (int i = 0; i < L; i++) {
        const int p = L - 1 - i;   // power of x
        const int xinv = pw(-(long long)p * prim_);
        int v = 0, xp = 1;
        for (int c : lam) { v ^= mul(c, xp); xp = mul(xp, xinv); }
        if (v == 0) where.push_back(i);
    }
    if ((int)where.size() != nerr) return -1;
    // Forney: e = X^(1 - fcr) Omega(X^-1) / Lambda'(X^-1)
    std::vector<int> om((size_t)nk, 0);
    for (size_t i = 0; i < lam.size(); i++)
        for (int j = 0; j < nk; j++) if ((int)i + j < nk) om[i + (size_t)j] ^= mul(lam[i], S[(size_t)j]);
    for (int i : where) {
        const int p = L - 1 - i;
        const int xinv = pw(-(long long)p * prim_);
        int o = 0, xp = 1;
        for (int c : om) { o ^= mul(c, xp); xp = mul(xp, xinv); }
        int dl = 0;
        xp = 1;
        for (size_t j = 1; j < lam.size(); j += 2) { dl ^= mul(lam[j], xp); xp = mul(xp, mul(xinv, xinv)); }
        if (dl == 0) return -1;
        int e = mul(o, exp_[(size_t)((nn_ - log_[(size_t)dl]) % nn_)]);
        e = mul(e, pw((long long)p * prim_ * (1 - fcr_)));
        cw[(size_t)i] ^= e;
    }
    // check
    for (int j = 0; j < nk; j++) {
        const int root = pw((long long)(fcr_ + j) * prim_);
        int s = 0;
        for (int c : cw) s = mul(s, root) ^ (c & nn_);
        if (s) return -1;
    }
    return nerr;
}

namespace stage {
namespace {

int parity(uint32_t v) { v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return (int)(v & 1); }
int pcnt(unsigned v) { int c = 0; while (v) { v &= v - 1; c++; } return c; }
int hamBase(const Definition& d) { return d.hamExtended ? d.hamN - 1 : d.hamN; }

std::vector<uint8_t> hamEncodeWord(const Definition& d, const uint8_t* data) {
    const int n0 = hamBase(d);
    std::vector<uint8_t> c((size_t)n0 + 1, 0);
    int di = 0;
    for (int i = 1; i <= n0; i++) if (i & (i - 1)) c[(size_t)i] = data[di++] & 1;
    for (int p = 1; p <= n0; p <<= 1) {
        int x = 0;
        for (int i = 1; i <= n0; i++) if ((i & p) && i != p) x ^= c[(size_t)i];
        c[(size_t)p] = (uint8_t)x;
    }
    std::vector<uint8_t> out(c.begin() + 1, c.end());
    if (d.hamExtended) { int x = 0; for (uint8_t b : out) x ^= b; out.push_back((uint8_t)x); }
    return out;
}
// false: two errors seen (extended code)
bool hamDecodeWord(const Definition& d, std::vector<uint8_t>& w, uint8_t* data, int& corrected) {
    const int n0 = hamBase(d);
    int syn = 0, overall = 0;
    for (int i = 1; i <= n0; i++) if (w[(size_t)(i - 1)] & 1) syn ^= i;
    for (uint8_t b : w) overall ^= b & 1;
    bool ok = true;
    if (d.hamExtended) {
        if (syn && overall) { w[(size_t)(syn - 1)] ^= 1; corrected++; }
        else if (syn && !overall) ok = false;
        else if (!syn && overall) corrected++;
    } else if (syn && syn <= n0) { w[(size_t)(syn - 1)] ^= 1; corrected++; }
    int di = 0;
    for (int i = 1; i <= n0; i++) if (i & (i - 1)) data[di++] = w[(size_t)(i - 1)];
    return ok;
}

std::vector<uint8_t> convEncode(const Definition& d, const std::vector<uint8_t>& in) {
    const int K = d.convK;
    uint32_t reg = 0;
    std::vector<uint8_t> o;
    o.reserve((in.size() + (size_t)K) * d.convPolys.size());
    auto step = [&](int b) {
        reg = (reg >> 1) | ((uint32_t)(b & 1) << (K - 1));
        for (uint32_t g : d.convPolys) o.push_back((uint8_t)parity(reg & g));
    };
    for (uint8_t b : in) step(b);
    if (d.convTail) for (int i = 0; i < K - 1; i++) step(0);
    return o;
}

// hard-decision Viterbi; returns the inputs (data and tail)
std::vector<uint8_t> viterbi(const Definition& d, const std::vector<uint8_t>& coded, size_t steps) {
    const int K = d.convK;
    const int np = (int)d.convPolys.size();
    const int S = 1 << (K - 1);
    const uint32_t smask = (uint32_t)S - 1;
    std::vector<uint8_t> outBits((size_t)1 << K);
    for (uint32_t reg = 0; reg < (1u << K); reg++) {
        uint8_t v = 0;
        for (int j = 0; j < np; j++) v = (uint8_t)((v << 1) | parity(reg & d.convPolys[(size_t)j]));
        outBits[reg] = v;
    }
    const int words = (S + 63) / 64;
    std::vector<uint64_t> dec(steps * (size_t)words, 0);
    const int big = 1 << 28;
    std::vector<int> pm((size_t)S, big), nm((size_t)S);
    pm[0] = 0;
    for (size_t t = 0; t < steps; t++) {
        uint8_t rx = 0;
        for (int j = 0; j < np; j++) { const size_t idx = t * (size_t)np + (size_t)j; rx = (uint8_t)((rx << 1) | (idx < coded.size() ? (coded[idx] & 1) : 0)); }
        for (int ns = 0; ns < S; ns++) {
            const uint32_t b = (uint32_t)ns >> (K - 2);
            const uint32_t s0 = ((uint32_t)ns << 1) & smask, s1 = s0 | 1;
            const uint32_t r0 = s0 | (b << (K - 1)), r1 = s1 | (b << (K - 1));
            const int m0 = pm[s0] + pcnt((unsigned)(outBits[r0] ^ rx));
            const int m1 = pm[s1] + pcnt((unsigned)(outBits[r1] ^ rx));
            if (m1 < m0) { nm[(size_t)ns] = m1; dec[t * (size_t)words + (size_t)(ns >> 6)] |= 1ull << (ns & 63); }
            else nm[(size_t)ns] = m0;
        }
        pm.swap(nm);
    }
    int st = 0;
    if (!d.convTail) { int best = big; for (int s = 0; s < S; s++) if (pm[(size_t)s] < best) { best = pm[(size_t)s]; st = s; } }
    std::vector<uint8_t> in(steps);
    for (size_t t = steps; t-- > 0;) {
        in[t] = (uint8_t)((uint32_t)st >> (K - 2));
        const int x = (int)((dec[t * (size_t)words + (size_t)(st >> 6)] >> (st & 63)) & 1);
        st = (int)((((uint32_t)st << 1) & smask) | (uint32_t)x);
    }
    return in;
}

}   // namespace

size_t codedBits(const Definition& d, size_t dataBits) {
    switch (d.fec) {
    case Fec::None: return dataBits;
    case Fec::Repetition: return dataBits * (size_t)d.repN;
    case Fec::Hamming: return (dataBits + (size_t)d.hamK - 1) / (size_t)d.hamK * (size_t)d.hamN;
    case Fec::Convolutional: return (dataBits + (d.convTail ? (size_t)d.convK - 1 : 0)) * d.convPolys.size();
    case Fec::ReedSolomon: {
        const size_t m = (size_t)d.rsM, syms = (dataBits + m - 1) / m, k = (size_t)d.rsK;
        const size_t blocks = (syms + k - 1) / k;
        return (syms + blocks * (size_t)(d.rsN - d.rsK)) * m;
    }
    }
    return dataBits;
}

std::vector<uint8_t> fecEncode(const Definition& d, const std::vector<uint8_t>& in) {
    std::vector<uint8_t> o;
    switch (d.fec) {
    case Fec::None: return in;
    case Fec::Repetition:
        for (uint8_t b : in) for (int i = 0; i < d.repN; i++) o.push_back(b & 1);
        return o;
    case Fec::Hamming: {
        std::vector<uint8_t> blk((size_t)d.hamK);
        for (size_t i = 0; i < in.size(); i += (size_t)d.hamK) {
            for (size_t k = 0; k < (size_t)d.hamK; k++) blk[k] = i + k < in.size() ? in[i + k] : 0;
            const std::vector<uint8_t> w = hamEncodeWord(d, blk.data());
            o.insert(o.end(), w.begin(), w.end());
        }
        return o;
    }
    case Fec::Convolutional: return convEncode(d, in);
    case Fec::ReedSolomon: {
        ReedSolomon rs(d.rsM, d.rsPoly, d.rsN, d.rsK, d.rsFcr, d.rsPrim);
        const size_t m = (size_t)d.rsM;
        std::vector<int> syms;
        for (size_t i = 0; i < in.size(); i += m) {
            int v = 0;
            for (size_t k = 0; k < m; k++) v = (v << 1) | (i + k < in.size() ? (in[i + k] & 1) : 0);
            syms.push_back(v);
        }
        for (size_t s = 0; s < syms.size(); s += (size_t)d.rsK) {
            const std::vector<int> blk(syms.begin() + (std::ptrdiff_t)s, syms.begin() + (std::ptrdiff_t)std::min(syms.size(), s + (size_t)d.rsK));
            for (int v : rs.encode(blk)) for (int k = (int)m - 1; k >= 0; k--) o.push_back((uint8_t)((v >> k) & 1));
        }
        return o;
    }
    }
    return in;
}

bool fecDecode(const Definition& d, const std::vector<uint8_t>& coded, size_t dataBits, std::vector<uint8_t>& out, int& corrected) {
    corrected = 0;
    out.assign(dataBits, 0);
    auto bit = [&](size_t i) -> uint8_t { return i < coded.size() ? (coded[i] & 1) : 0; };
    switch (d.fec) {
    case Fec::None:
        for (size_t i = 0; i < dataBits; i++) out[i] = bit(i);
        return true;
    case Fec::Repetition:
        for (size_t i = 0; i < dataBits; i++) {
            int ones = 0;
            for (int k = 0; k < d.repN; k++) ones += bit(i * (size_t)d.repN + (size_t)k);
            out[i] = (uint8_t)(ones * 2 > d.repN);
            corrected += out[i] ? d.repN - ones : ones;
        }
        return true;
    case Fec::Hamming: {
        bool ok = true;
        std::vector<uint8_t> w((size_t)d.hamN), data((size_t)d.hamK);
        size_t o = 0;
        for (size_t cw = 0; o < dataBits; cw++) {
            for (size_t k = 0; k < (size_t)d.hamN; k++) w[k] = bit(cw * (size_t)d.hamN + k);
            ok &= hamDecodeWord(d, w, data.data(), corrected);
            for (size_t k = 0; k < (size_t)d.hamK && o < dataBits; k++) out[o++] = data[k];
        }
        return ok;
    }
    case Fec::Convolutional: {
        const size_t steps = dataBits + (d.convTail ? (size_t)d.convK - 1 : 0);
        const std::vector<uint8_t> in = viterbi(d, coded, steps);
        for (size_t i = 0; i < dataBits; i++) out[i] = in[i];
        const std::vector<uint8_t> re = convEncode(d, in.size() > dataBits && d.convTail ? std::vector<uint8_t>(in.begin(), in.begin() + (std::ptrdiff_t)dataBits) : in);
        for (size_t i = 0; i < re.size() && i < coded.size(); i++) corrected += (re[i] ^ coded[i]) & 1;
        return true;
    }
    case Fec::ReedSolomon: {
        ReedSolomon rs(d.rsM, d.rsPoly, d.rsN, d.rsK, d.rsFcr, d.rsPrim);
        const size_t m = (size_t)d.rsM, syms = (dataBits + m - 1) / m, k = (size_t)d.rsK, nk = (size_t)(d.rsN - d.rsK);
        size_t cpos = 0, o = 0;
        bool ok = true;
        for (size_t s = 0; s < syms; s += k) {
            const size_t len = std::min(k, syms - s) + nk;
            std::vector<int> cw(len);
            for (size_t i = 0; i < len; i++) {
                int v = 0;
                for (size_t b = 0; b < m; b++) v = (v << 1) | bit(cpos++);
                cw[i] = v;
            }
            const int r = rs.decode(cw);
            if (r < 0) ok = false;
            else corrected += r;
            for (size_t i = 0; i + nk < len; i++)
                for (int b = (int)m - 1; b >= 0; b--) if (o < dataBits) out[o++] = (uint8_t)((cw[i] >> b) & 1);
        }
        return ok;
    }
    }
    return true;
}

}   // namespace stage
}   // namespace dect2::modebuilder
