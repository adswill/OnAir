// DTMB LDPC codes, BCH, 4QAM-NR decoding (see dtmb_ldpc.h).
#include "dect2/dtmb_ldpc.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace dect2::dtmb {

namespace {
constexpr uint64_t kMaskHi = 0x7FFFFFFFFFFFFFFFull;   // a block has 127 bits: 64 in lo, 63 in hi

struct Bits { uint64_t lo = 0, hi = 0; };

// result bit i = v bit (i + s) mod 127
inline Bits rotr(Bits v, int s) {
    if (s == 0) return v;
    Bits a, b;
    if (s < 64) { a.lo = (v.lo >> s) | (v.hi << (64 - s)); a.hi = v.hi >> s; }
    else { a.lo = v.hi >> (s - 64); a.hi = 0; }
    const int t = 127 - s;
    if (t < 64) { b.hi = (v.hi << t) | (v.lo >> (64 - t)); b.lo = v.lo << t; }
    else { b.hi = v.lo << (t - 64); b.lo = 0; }
    Bits r;
    r.lo = a.lo | b.lo;
    r.hi = (a.hi | b.hi) & kMaskHi;
    return r;
}
inline Bits operator^(Bits a, Bits b) { return Bits{a.lo ^ b.lo, a.hi ^ b.hi}; }
inline bool zero(Bits a) { return (a.lo | a.hi) == 0; }
inline int bitAt(const Bits& v, int i) { return i < 64 ? (int)((v.lo >> i) & 1) : (int)((v.hi >> (i - 64)) & 1); }
inline void setBit(Bits& v, int i) { if (i < 64) v.lo |= 1ull << i; else v.hi |= 1ull << (i - 64); }
} // namespace

// ---------------------------------------------------------------- LDPC
LdpcCode::LdpcCode(Rate r) : rate_(r) {
    int n = 0, rows = 0;
    const LdpcBlock* b = ldpcBlocks(r, n, rows);
    blocks_.assign(b, b + n);
    c_ = rows;
    e_ = 59 - rows;
    rows_.assign((size_t)c_, {});
    for (const LdpcBlock& k : blocks_) rows_[k.row].push_back(Edge{k.col, k.shift});
    edgeBase_.assign((size_t)c_ + 1, 0);
    for (int i = 0; i < c_; i++) edgeBase_[(size_t)i + 1] = edgeBase_[(size_t)i] + (int)rows_[(size_t)i].size();
    planEncoder();
}

// Parity part D of H = [D | C]: find an order in which each block row yields one new unknown block; the few that cannot be peeled
// (the gap, three blocks for these codes) are solved at the end from the rows that were not used, with a small dense GF(2) inverse.
void LdpcCode::planEncoder() {
    std::vector<char> solved((size_t)c_, 0), used((size_t)c_, 0);
    gap_.clear(); order_.clear(); left_.clear();
    int nSolved = 0;
    while (nSolved < c_) {
        bool progress = true;
        while (progress) {
            progress = false;
            for (int r = 0; r < c_; r++) {
                if (used[(size_t)r]) continue;
                int unknown = -1, count = 0;
                for (const Edge& e : rows_[(size_t)r]) if (e.col < c_ && !solved[(size_t)e.col]) { unknown = e.col; count++; }
                if (count == 1) { solved[(size_t)unknown] = 1; used[(size_t)r] = 1; order_.push_back({r, unknown}); nSolved++; progress = true; }
            }
        }
        if (nSolved >= c_) break;
        std::vector<int> cnt((size_t)c_, 0);
        for (int r = 0; r < c_; r++) {
            if (used[(size_t)r]) continue;
            for (const Edge& e : rows_[(size_t)r]) if (e.col < c_ && !solved[(size_t)e.col]) cnt[(size_t)e.col]++;
        }
        int best = -1;
        for (int j = 0; j < c_; j++) if (!solved[(size_t)j] && (best < 0 || cnt[(size_t)j] > cnt[(size_t)best])) best = j;
        gap_.push_back(best); solved[(size_t)best] = 1; nSolved++;
    }
    for (int r = 0; r < c_; r++) if (!used[(size_t)r]) left_.push_back(r);
    const int G = (int)gap_.size();
    if ((int)left_.size() != G || G * kLdpcZ > 384) throw std::runtime_error("DTMB LDPC: encoder plan failed");
    // gap matrix: residual of the left rows for each unit vector of the gap blocks, with an all-zero message
    const int bitsN = G * kLdpcZ;
    std::vector<std::vector<uint64_t>> aug((size_t)bitsN, std::vector<uint64_t>(12, 0));   // [A | I], 6 words each
    std::vector<Blk> zeroS((size_t)c_), g((size_t)G), p, resid;
    for (int cj = 0; cj < bitsN; cj++) {
        for (auto& x : g) x = Blk{0, 0};
        const int gi = cj / kLdpcZ, bit = cj % kLdpcZ;
        if (bit < 64) g[(size_t)gi].lo = 1ull << bit; else g[(size_t)gi].hi = 1ull << (bit - 64);
        run(zeroS, g, p, resid);
        for (int li = 0; li < G; li++) {
            Bits v{resid[(size_t)li].lo, resid[(size_t)li].hi};
            for (int i = 0; i < kLdpcZ; i++) if (bitAt(v, i)) { const int ri = li * kLdpcZ + i; aug[(size_t)ri][(size_t)(cj / 64)] |= 1ull << (cj % 64); }
        }
    }
    for (int i = 0; i < bitsN; i++) aug[(size_t)i][(size_t)(6 + i / 64)] |= 1ull << (i % 64);
    for (int col = 0; col < bitsN; col++) {
        int piv = -1;
        for (int r = col; r < bitsN; r++) if ((aug[(size_t)r][(size_t)(col / 64)] >> (col % 64)) & 1) { piv = r; break; }
        if (piv < 0) throw std::runtime_error("DTMB LDPC: gap matrix is singular");
        std::swap(aug[(size_t)piv], aug[(size_t)col]);
        for (int r = 0; r < bitsN; r++) {
            if (r != col && ((aug[(size_t)r][(size_t)(col / 64)] >> (col % 64)) & 1))
                for (int w = 0; w < 12; w++) aug[(size_t)r][(size_t)w] ^= aug[(size_t)col][(size_t)w];
        }
    }
    ainv_.assign((size_t)bitsN, std::vector<uint64_t>(6, 0));
    for (int i = 0; i < bitsN; i++) for (int w = 0; w < 6; w++) ainv_[(size_t)i][(size_t)w] = aug[(size_t)i][(size_t)(6 + w)];
}

// Solves the pivot rows in order for the unknown parity blocks, given the syndrome contribution s of the information blocks and values for the gap
// blocks; resid receives the sums of the rows that were not used (zero when the gap values are right).
void LdpcCode::run(const std::vector<Blk>& s, const std::vector<Blk>& g, std::vector<Blk>& p, std::vector<Blk>& resid) const {
    p.assign((size_t)c_, Blk{0, 0});
    for (size_t i = 0; i < gap_.size(); i++) p[(size_t)gap_[i]] = g[i];
    for (const auto& step : order_) {
        const int r = step.first, j = step.second;
        Bits acc{s[(size_t)r].lo, s[(size_t)r].hi};
        int sj = 0;
        for (const Edge& e : rows_[(size_t)r]) {
            if (e.col >= c_) continue;
            if (e.col == j) { sj = e.shift; continue; }
            acc = acc ^ rotr(Bits{p[(size_t)e.col].lo, p[(size_t)e.col].hi}, e.shift);
        }
        const Bits v = rotr(acc, (kLdpcZ - sj) % kLdpcZ);
        p[(size_t)j] = Blk{v.lo, v.hi};
    }
    resid.assign(left_.size(), Blk{0, 0});
    for (size_t li = 0; li < left_.size(); li++) {
        const int r = left_[li];
        Bits acc{s[(size_t)r].lo, s[(size_t)r].hi};
        for (const Edge& e : rows_[(size_t)r]) if (e.col < c_) acc = acc ^ rotr(Bits{p[(size_t)e.col].lo, p[(size_t)e.col].hi}, e.shift);
        resid[li] = Blk{acc.lo, acc.hi};
    }
}

void LdpcCode::encode(const uint8_t* info, uint8_t* sent) const {
    std::vector<uint8_t> w((size_t)kLdpcVars);
    encodeFull(info, w.data());
    std::memcpy(sent, w.data() + 5, (size_t)(kLdpcVars - 5));
}

void LdpcCode::encodeFull(const uint8_t* info, uint8_t* word) const {
    std::vector<Bits> m((size_t)e_);
    for (int j = 0; j < e_; j++) {
        Bits b;
        for (int i = 0; i < kLdpcZ; i++) if (info[j * kLdpcZ + i]) setBit(b, i);
        m[(size_t)j] = b;
    }
    std::vector<Blk> s((size_t)c_, Blk{0, 0});
    for (int r = 0; r < c_; r++) {
        Bits acc;
        for (const Edge& e : rows_[(size_t)r]) if (e.col >= c_) acc = acc ^ rotr(m[(size_t)(e.col - c_)], e.shift);
        s[(size_t)r] = Blk{acc.lo, acc.hi};
    }
    const int G = (int)gap_.size();
    std::vector<Blk> g((size_t)G, Blk{0, 0}), p, resid;
    run(s, g, p, resid);
    // gap values: ainv * resid
    std::vector<uint64_t> bvec(6, 0);
    for (int li = 0; li < G; li++) {
        Bits v{resid[(size_t)li].lo, resid[(size_t)li].hi};
        for (int i = 0; i < kLdpcZ; i++) if (bitAt(v, i)) { const int ri = li * kLdpcZ + i; bvec[(size_t)(ri / 64)] |= 1ull << (ri % 64); }
    }
    for (int gi = 0; gi < G; gi++) {
        Bits v;
        for (int i = 0; i < kLdpcZ; i++) {
            const auto& row = ainv_[(size_t)(gi * kLdpcZ + i)];
            uint64_t acc = 0;
            for (int w = 0; w < 6; w++) acc ^= row[(size_t)w] & bvec[(size_t)w];
            if (std::popcount(acc) & 1) setBit(v, i);
        }
        g[(size_t)gi] = Blk{v.lo, v.hi};
    }
    run(s, g, p, resid);
    int t = 0;
    for (int v = 0; v < c_ * kLdpcZ; v++) word[t++] = (uint8_t)bitAt(Bits{p[(size_t)(v / kLdpcZ)].lo, p[(size_t)(v / kLdpcZ)].hi}, v % kLdpcZ);
    std::memcpy(word + t, info, (size_t)e_ * kLdpcZ);
}

int LdpcCode::syndromeWeight(const uint8_t* word) const {
    int bad = 0;
    for (int r = 0; r < c_; r++) for (int i = 0; i < kLdpcZ; i++) {
        int x = 0;
        for (const Edge& e : rows_[(size_t)r]) x ^= word[e.col * kLdpcZ + (i + e.shift) % kLdpcZ];
        bad += x;
    }
    return bad;
}

LdpcCode::Decoder::Decoder(const LdpcCode& code) : code_(code) {
    lv_.assign(59 * 128, 0.f);
    r_.assign((size_t)code.blocks_.size() * 128, 0.f);
    size_t maxDeg = 0;
    for (const auto& r : code.rows_) maxDeg = std::max(maxDeg, r.size());
    q_.assign(maxDeg * 128, 0.f);
    hard_.assign(59 * 2, 0);
}

LdpcCode::Result LdpcCode::Decoder::decode(const float* llr, uint8_t* info, int maxIter, float alpha) {
    const int c = code_.c_, e = code_.e_;
    constexpr int Z = kLdpcZ;
    // The a posteriori values are clipped far above their useful range. A tight clip (40 was tried) breaks the layered update, which subtracts
    // the stored check message from the clipped total: a word that had converged to its information bits could then diverge to the wrong
    // word within a few iterations (about 1 in 1000 words at 3 dB above the threshold).
    constexpr float kClip = 1000.f;
    for (int col = 0; col < 59; col++) {
        float* lv = &lv_[(size_t)col * 128];
        for (int i = 0; i < Z; i++) {
            const int v = col * Z + i;
            lv[i] = v < 5 ? 0.f : std::max(-kClip, std::min(kClip, llr[v - 5]));
        }
        lv[Z] = 0.f;
    }
    std::fill(r_.begin(), r_.end(), 0.f);
    Result res;
    {   // mostly erased input
        int zeros = 0;
        for (int i = 0; i < kLdpcSent; i++) zeros += llr[i] == 0.f;
        if (zeros > kLdpcSent / 3) {
            for (int j = 0; j < e; j++) for (int i = 0; i < Z; i++) info[j * Z + i] = (uint8_t)(lv_[(size_t)(c + j) * 128 + (size_t)i] < 0.f);
            res.iterations = 1;
            return res;
        }
    }
    float min1[128], min2[128];
    int idx[128];
    uint32_t sg[128];
    for (int iter = 1; iter <= maxIter; iter++) {
        for (int row = 0; row < c; row++) {
            const auto& edges = code_.rows_[(size_t)row];
            const int deg = (int)edges.size();
            const int eBase = code_.edgeBase_[(size_t)row];
            for (int i = 0; i < Z; i++) { min1[i] = std::numeric_limits<float>::max(); min2[i] = min1[i]; idx[i] = -1; sg[i] = 0; }
            for (int b = 0; b < deg; b++) {
                const float* lv = &lv_[(size_t)edges[(size_t)b].col * 128];
                const float* ro = &r_[(size_t)(eBase + b) * 128];
                float* q = &q_[(size_t)b * 128];
                const int sh = edges[(size_t)b].shift;
                const int n1 = Z - sh;
                for (int i = 0; i < n1; i++) q[i] = lv[i + sh] - ro[i];
                for (int i = n1; i < Z; i++) q[i] = lv[i + sh - Z] - ro[i];
                for (int i = 0; i < Z; i++) {
                    const float a = std::fabs(q[i]);
                    const bool lt1 = a < min1[i];
                    const bool lt2 = a < min2[i];
                    min2[i] = lt1 ? min1[i] : (lt2 ? a : min2[i]);
                    idx[i] = lt1 ? b : idx[i];
                    min1[i] = lt1 ? a : min1[i];
                    sg[i] ^= std::bit_cast<uint32_t>(q[i]) & 0x80000000u;
                }
            }
            for (int b = 0; b < deg; b++) {
                float* lv = &lv_[(size_t)edges[(size_t)b].col * 128];
                float* rn = &r_[(size_t)(eBase + b) * 128];
                float* q = &q_[(size_t)b * 128];
                const int sh = edges[(size_t)b].shift;
                const int n1 = Z - sh;
                for (int i = 0; i < Z; i++) {
                    const float m = (idx[i] == b ? min2[i] : min1[i]) * alpha;
                    const uint32_t sign = sg[i] ^ (std::bit_cast<uint32_t>(q[i]) & 0x80000000u);
                    const float v = std::bit_cast<float>(std::bit_cast<uint32_t>(m) | sign);
                    rn[i] = v;
                    q[i] += v;
                }
                for (int i = 0; i < n1; i++) lv[i + sh] = std::max(-kClip, std::min(kClip, q[i]));
                for (int i = n1; i < Z; i++) lv[i + sh - Z] = std::max(-kClip, std::min(kClip, q[i]));
            }
        }
        // hard decisions and the parity checks
        for (int col = 0; col < 59; col++) {
            const float* lv = &lv_[(size_t)col * 128];
            uint64_t lo = 0, hi = 0;
            for (int i = 0; i < 64; i++) lo |= (uint64_t)(lv[i] < 0.f) << i;
            for (int i = 64; i < Z; i++) hi |= (uint64_t)(lv[i] < 0.f) << (i - 64);
            hard_[(size_t)col * 2] = lo; hard_[(size_t)col * 2 + 1] = hi;
        }
        int badRows = 0;
        for (int row = 0; row < c; row++) {
            Bits acc;
            for (const Edge& ed : code_.rows_[(size_t)row]) acc = acc ^ rotr(Bits{hard_[(size_t)ed.col * 2], hard_[(size_t)ed.col * 2 + 1]}, ed.shift);
            if (!zero(acc)) { badRows++; if (iter < 12) break; }   // before iteration 12 only whether all checks hold matters
        }
        res.iterations = iter;
        if (badRows == 0) { res.ok = true; break; }
        if (iter == 12) {
            int weight = 0;
            for (int row = 0; row < c; row++) {
                Bits acc;
                for (const Edge& ed : code_.rows_[(size_t)row]) acc = acc ^ rotr(Bits{hard_[(size_t)ed.col * 2], hard_[(size_t)ed.col * 2 + 1]}, ed.shift);
                weight += std::popcount(acc.lo) + std::popcount(acc.hi);
            }
            if (weight > code_.c_ * Z / 3) break;
        }
    }
    for (int j = 0; j < e; j++) for (int i = 0; i < Z; i++) info[j * Z + i] = (uint8_t)(lv_[(size_t)(c + j) * 128 + (size_t)i] < 0.f);
    return res;
}

const LdpcCode& ldpcCode(Rate r) {
    static std::once_flag once;
    static std::unique_ptr<LdpcCode> codes[3];
    std::call_once(once, [] { for (int i = 0; i < 3; i++) codes[i] = std::make_unique<LdpcCode>((Rate)i); });
    return *codes[(int)r];
}

// ---------------------------------------------------------------- BCH
namespace {
constexpr uint32_t kBchPoly = 0x409;   // x^10 + x^3 + 1
uint32_t bchRem(const uint8_t* bits, int n, int extraZeros) {
    uint32_t rem = 0;
    for (int i = 0; i < n + extraZeros; i++) {
        rem = (rem << 1) | (i < n ? bits[i] : 0u);
        if (rem & 0x400u) rem ^= kBchPoly;
    }
    return rem;
}
const std::vector<int16_t>& bchSyndromeTable() {
    static const std::vector<int16_t> t = [] {
        std::vector<int16_t> v(1024, -1);
        std::vector<uint8_t> w((size_t)kBchN, 0);
        for (int p = 0; p < kBchN; p++) {
            std::fill(w.begin(), w.end(), 0);
            w[(size_t)p] = 1;
            v[bchRem(w.data(), kBchN, 0)] = (int16_t)p;
        }
        return v;
    }();
    return t;
}
} // namespace

void bchEncode(const uint8_t* msg, uint8_t* word) {
    std::memcpy(word, msg, kBchK);
    const uint32_t rem = bchRem(msg, kBchK, 10);
    for (int i = 0; i < 10; i++) word[kBchK + i] = (uint8_t)((rem >> (9 - i)) & 1);
}

int bchDecode(uint8_t* word) {
    const uint32_t syn = bchRem(word, kBchN, 0);
    if (syn == 0) return 0;
    const int p = bchSyndromeTable()[syn];
    if (p < 0) return -1;
    word[p] ^= 1;
    return 1;
}

// ---------------------------------------------------------------- 4QAM-NR
void nrSoftDecode(const float* l, float* out) {
    static const std::array<uint8_t, 256> par = [] {
        std::array<uint8_t, 256> t{};
        for (int v = 0; v < 256; v++) t[(size_t)v] = nrParity((uint8_t)v);
        return t;
    }();
    // the cost of a candidate is the sum of the LLRs at its one bits: tables of 16 per nibble of the byte and of its parity byte
    float a_lo[16], a_hi[16], b_lo[16], b_hi[16];
    a_lo[0] = a_hi[0] = b_lo[0] = b_hi[0] = 0.f;
    for (int v = 1; v < 16; v++) {
        const int bit = std::countr_zero((unsigned)v), prev = v & (v - 1);
        a_lo[v] = a_lo[prev] + l[7 - bit];
        a_hi[v] = a_hi[prev] + l[7 - (bit + 4)];
        b_lo[v] = b_lo[prev] + l[15 - bit];
        b_hi[v] = b_hi[prev] + l[15 - (bit + 4)];
    }
    float cost[256];
    for (int h = 0; h < 16; h++) for (int j = 0; j < 16; j++) {
        const int v = h * 16 + j, y = par[(size_t)v];
        cost[v] = a_hi[h] + a_lo[j] + b_hi[y >> 4] + b_lo[y & 15];
    }
    // minima over the words with each bit clear and set: reduce the high nibble away to rows of 16, then the low nibble to 16 values
    float rowMin[16], colMin[16];
    for (int j = 0; j < 16; j++) rowMin[j] = cost[j];
    for (int h = 1; h < 16; h++) for (int j = 0; j < 16; j++) rowMin[j] = std::min(rowMin[j], cost[h * 16 + j]);
    for (int h = 0; h < 16; h++) {
        float m = cost[h * 16];
        for (int j = 1; j < 16; j++) m = std::min(m, cost[h * 16 + j]);
        colMin[h] = m;
    }
    for (int bit = 0; bit < 8; bit++) {   // bit of the byte; out[7 - bit]
        const float* src = bit < 4 ? rowMin : colMin;
        const int sh = bit < 4 ? bit : bit - 4;
        float m0 = std::numeric_limits<float>::max(), m1 = m0;
        for (int v = 0; v < 16; v++) { if ((v >> sh) & 1) m1 = std::min(m1, src[v]); else m0 = std::min(m0, src[v]); }
        out[7 - bit] = m1 - m0;
    }
}

} // namespace dect2::dtmb
