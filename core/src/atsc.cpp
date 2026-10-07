#include "dect2/atsc.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <vector>

namespace dect2 {
namespace atsc {

const uint8_t kPn511[511] = {
    0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 1, 0,
    0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 0, 1,
    0, 1, 1, 1, 1, 1, 0, 1, 0, 0, 1, 1, 0, 1, 0, 1, 0, 0, 1, 1, 1, 0, 1, 1, 0, 0, 1, 1, 1, 0, 1, 0,
    0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 1, 1, 1,
    1, 1, 0, 0, 1, 1, 1, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 1,
    0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0,
    1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0,
    0, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 0, 0, 1, 1, 1, 0,
    0, 1, 1, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 1, 1, 1, 0, 1, 1,
    0, 0, 0, 1, 0, 1, 0, 1, 1, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1,
    1, 0, 0, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 1, 1, 1, 0, 0, 0, 1, 1,
    1, 0, 0, 1, 0, 1, 1, 1, 1, 0, 1, 0, 0, 0, 1, 1, 0, 1, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1,
    1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0,
    1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0,
    1, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 0, 0, 0, 0, 1, 0, 1, 1, 0, 1,
    0, 0, 0, 0, 0, 1, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0,
};
const uint8_t kPn63[63] = {
    1, 1, 1, 0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 1, 0, 1, 1, 1, 1,
    1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1, 0, 1, 0, 0, 0,
};

void fieldSyncSymbols(bool field2, const uint8_t prev12[12], uint8_t* p) {
    static const uint8_t bin[2] = {1, 6};   // binary value -> symbol (-5 / +5)
    int i = 0;
    p[i++] = bin[1]; p[i++] = bin[0]; p[i++] = bin[0]; p[i++] = bin[1];   // segment sync
    for (int j = 0; j < 511; j++) p[i++] = bin[kPn511[j]];
    for (int j = 0; j < 63; j++) p[i++] = bin[kPn63[j]];
    for (int j = 0; j < 63; j++) p[i++] = bin[kPn63[j] ^ (field2 ? 1 : 0)];
    for (int j = 0; j < 63; j++) p[i++] = bin[kPn63[j]];
    // 24 bits of VSB mode identification (8-VSB): 0000 1010 0101 1111 0101 1010
    static const uint8_t mode[24] = {0,0,0,0, 1,0,1,0, 0,1,0,1, 1,1,1,1, 0,1,0,1, 1,0,1,0};
    for (int j = 0; j < 24; j++) p[i++] = bin[mode[j]];
    for (int j = 0; j < 92; j++) p[i++] = bin[kPn63[j % 63]];                // reserved
    for (int j = 0; j < 12; j++) p[i++] = prev12[j];                          // pre-code
}

// ---------------------------------------------------------------- randomiser
void Randomizer::apply(uint8_t* data, size_t n) {
    for (size_t k = 0; k < n; k++) {
        const unsigned st = state_;
        unsigned o = 0;
        if (st & 0x8000) o |= 0x01;
        if (st & 0x2000) o |= 0x02;
        if (st & 0x1000) o |= 0x04;
        if (st & 0x0200) o |= 0x08;
        if (st & 0x0020) o |= 0x10;
        if (st & 0x0010) o |= 0x20;
        if (st & 0x0008) o |= 0x40;
        if (st & 0x0004) o |= 0x80;
        data[k] ^= (uint8_t)o;
        state_ = (st & 1) ? (((st ^ 0xa638u) >> 1) | 0x8000u) : (st >> 1);
    }
}

// ---------------------------------------------------------------- Reed-Solomon (207,187)
namespace {
struct Gf {
    uint8_t exp[512], log[256];
    Gf() {
        unsigned x = 1;
        for (int i = 0; i < 255; i++) { exp[i] = (uint8_t)x; log[x] = (uint8_t)i; x <<= 1; if (x & 0x100) x ^= 0x11d; }
        for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
        log[0] = 0;
    }
    uint8_t mul(uint8_t a, uint8_t b) const { return (a && b) ? exp[log[a] + log[b]] : 0; }
    uint8_t inv(uint8_t a) const { return exp[255 - log[a]]; }
};
const Gf& gf() { static const Gf g; return g; }
constexpr int kRoots = 20;
constexpr int kN = 207;
struct RsGen {
    uint8_t g[kRoots + 1];   // g[0] x^20 + ... (highest degree first)
    RsGen() {
        const Gf& f = gf();
        uint8_t p[kRoots + 1] = {1};
        int deg = 0;
        for (int i = 0; i < kRoots; i++) {   // multiply by (x + alpha^i)
            uint8_t q[kRoots + 1] = {};
            for (int j = 0; j <= deg; j++) { q[j] ^= p[j]; q[j + 1] ^= f.mul(p[j], f.exp[i]); }
            deg++;
            memcpy(p, q, sizeof p);
        }
        memcpy(g, p, sizeof g);
    }
};
}

namespace {
// fb * g[j + 1] for every feedback byte fb: one row of 24 bytes (20 used, zero padded) per value, so that one division step is three
// 64-bit exclusive ors
struct RsRows {
    alignas(8) uint8_t row[256][24];
    RsRows() {
        const Gf& f = gf();
        const RsGen gen;
        memset(row, 0, sizeof row);
        for (int v = 0; v < 256; v++) for (int j = 0; j < kRoots; j++) row[v][j] = f.mul((uint8_t)v, gen.g[j + 1]);
    }
};
const RsRows& rsRows() { static const RsRows r; return r; }

// remainder of the 207 bytes in `b` (highest degree first) divided by the generator polynomial, written to rem[20]; `b` must have 24 bytes of
// room behind it (it is the working copy: its first 187 bytes are consumed)
inline void rsRemainder(uint8_t* b, uint8_t* rem) {
    const RsRows& R = rsRows();
    for (int i = 0; i < 187; i++) {
        const uint8_t fb = b[i];
        if (!fb) continue;
        uint64_t x[3], y[3];
        memcpy(x, b + i + 1, 24); memcpy(y, R.row[fb], 24);
        x[0] ^= y[0]; x[1] ^= y[1]; x[2] ^= y[2];
        memcpy(b + i + 1, x, 24);
    }
    memcpy(rem, b + 187, kRoots);
}
}

void rsEncode(const uint8_t* in, uint8_t* out) {
    uint8_t b[187 + 24 + 8] = {};
    memcpy(b, in, 187);
    uint8_t rem[kRoots];
    rsRemainder(b, rem);
    memcpy(out, in, 187);
    memcpy(out + 187, rem, kRoots);
}

int rsDecode(uint8_t* r) {
    const Gf& f = gf();
    uint8_t S[kRoots];
    bool any = false;
    {   // syndromes: the remainder of r(x) modulo g(x) is zero exactly when all of them are, and S_i = rem(alpha^i)
        uint8_t w[kN + 24 + 8] = {}, rem[kRoots];
        memcpy(w, r, kN);
        rsRemainder(w, rem);
        for (int j = 0; j < kRoots; j++) any |= rem[j] != 0;
        if (!any) return 0;
        for (int i = 0; i < kRoots; i++) {
            uint8_t s = 0;
            for (int j = 0; j < kRoots; j++) s = f.mul(s, f.exp[i]) ^ rem[j];
            S[i] = s;
        }
    }
    uint8_t C[kRoots + 1] = {1}, B[kRoots + 1] = {1};
    int L = 0, m = 1;
    uint8_t b = 1;
    for (int n = 0; n < kRoots; n++) {
        uint8_t d = S[n];
        for (int i = 1; i <= L; i++) d ^= f.mul(C[i], S[n - i]);
        if (d == 0) { m++; continue; }
        uint8_t T[kRoots + 1];
        memcpy(T, C, sizeof T);
        const uint8_t coef = f.mul(d, f.inv(b));
        for (int i = 0; i + m <= kRoots; i++) C[i + m] ^= f.mul(coef, B[i]);
        if (2 * L <= n) { L = n + 1 - L; memcpy(B, T, sizeof B); b = d; m = 1; } else m++;
    }
    if (L > kRoots / 2) return -1;
    int pos[kRoots / 2], cnt = 0;
    for (int p = 0; p < kN; p++) {
        const uint8_t x = f.exp[(255 - p) % 255];
        uint8_t v = 0;
        for (int i = L; i >= 0; i--) v = f.mul(v, x) ^ C[i];
        if (v == 0) { if (cnt >= kRoots / 2) return -1; pos[cnt++] = p; }
    }
    if (cnt != L) return -1;
    uint8_t om[kRoots] = {};
    for (int i = 0; i < kRoots; i++) { uint8_t s = 0; for (int j = 0; j <= std::min(i, L); j++) s ^= f.mul(C[j], S[i - j]); om[i] = s; }
    for (int k = 0; k < cnt; k++) {
        const int p = pos[k];
        const uint8_t X = f.exp[p % 255], xi = f.exp[(255 - p) % 255];
        uint8_t num = 0;
        for (int i = kRoots - 1; i >= 0; i--) num = f.mul(num, xi) ^ om[i];
        uint8_t den = 0;
        for (int i = 1; i <= L; i += 2) den ^= f.mul(C[i], f.exp[((255 - p) * (i - 1)) % 255]);
        if (den == 0) return -1;
        r[kN - 1 - p] ^= f.mul(f.mul(num, X), f.inv(den));
    }
    {
        uint8_t w[kN + 24 + 8] = {}, rem[kRoots];
        memcpy(w, r, kN);
        rsRemainder(w, rem);
        for (int j = 0; j < kRoots; j++) if (rem[j]) return -1;
    }
    return cnt;
}

// ---------------------------------------------------------------- byte interleaver
ByteInterleaver::ByteInterleaver(bool inverse) : inverse_(inverse) {
    fifo_.resize(kInterleave);
    for (int c = 0; c < kInterleave; c++) fifo_[c].v.assign(inverse ? (kInterleave - 1 - c) * 4 : c * 4, 0);
    if (inverse) align_.v.assign(156, 0);
}

void ByteInterleaver::reset() {
    comm_ = 0;
    for (auto& f : fifo_) { std::fill(f.v.begin(), f.v.end(), 0); f.pos = 0; }
    std::fill(align_.v.begin(), align_.v.end(), 0); align_.pos = 0;
}

void ByteInterleaver::process(const uint8_t* in, uint8_t* out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t b = fifo_[comm_].stuff(in[i]);
        if (++comm_ >= kInterleave) comm_ = 0;
        out[i] = inverse_ ? align_.stuff(b) : b;
    }
}

// ---------------------------------------------------------------- trellis
const TrellisTables& trellis() {
    static const TrellisTables t = {
        {0, 1, 4, 5, 2, 3, 6, 7, 1, 0, 5, 4, 3, 2, 7, 6, 4, 5, 0, 1, 6, 7, 2, 3, 5, 4, 1, 0, 7, 6, 3, 2},
        {0, 2, 4, 6, 1, 3, 5, 7, 0, 2, 4, 6, 1, 3, 5, 7, 4, 6, 0, 2, 5, 7, 1, 3, 4, 6, 0, 2, 5, 7, 1, 3}};
    return t;
}

const TrellisMap& trellisMap() {
    static const TrellisMap m = [] {
        TrellisMap t;
        const int IN = 12 * kSegBytes;
        int encoder = kNumEnc - 4, skip = 0, outpos = 0, nextSeg = 0;
        int bufByte[kNumEnc] = {};
        int cnt[kNumEnc] = {};
        for (int chunk = 0; chunk < IN; chunk += kNumEnc) {
            if (outpos >= nextSeg) { encoder = (encoder + 4) % kNumEnc; skip = 1; }
            for (int i = 0; i < kNumEnc; i++) { bufByte[encoder] = chunk + i; encoder = (encoder + 1) % kNumEnc; }
            for (int shift = 6; shift >= 0; shift -= 2) {
                if (outpos >= nextSeg) {
                    outpos += 4;
                    nextSeg = outpos + 828;
                    if (!skip) encoder = (encoder + 4) % kNumEnc;
                    skip = 0;
                }
                for (int i = 0; i < kNumEnc; i++) {
                    const int k = cnt[encoder]++;
                    t.symPos[encoder][k] = outpos;
                    t.byteIdx[encoder][k] = bufByte[encoder];
                    t.shift[encoder][k] = shift;
                    outpos++;
                    encoder = (encoder + 1) % kNumEnc;
                }
            }
        }
        return t;
    }();
    return m;
}

// ---------------------------------------------------------------- field encoder
FieldEncoder::FieldEncoder() {
    memset(encState_, 0, sizeof encState_);
    memset(last12_, 0, sizeof last12_);
}

void FieldEncoder::encode(const uint8_t* ts, bool field2, uint8_t* symbols) {
    Randomizer rnd;
    std::vector<uint8_t> rs(kDataSegs * kSegBytes), il(kDataSegs * kSegBytes);
    for (int p = 0; p < kDataSegs; p++) {
        uint8_t m[187];
        memcpy(m, ts + p * kTsBytes + 1, 187);
        rnd.apply(m, 187);
        rsEncode(m, &rs[p * kSegBytes]);
    }
    il_.syncCommutator();
    il_.process(rs.data(), il.data(), il.size());
    fieldSyncSymbols(field2, last12_, symbols);
    const TrellisTables& tt = trellis();
    const TrellisMap& tm = trellisMap();
    for (int blk = 0; blk < kDataSegs / 12; blk++) {
        uint8_t* out = symbols + (size_t)(1 + blk * 12) * kSegSyms;
        for (int s = 0; s < 12; s++) { uint8_t* g = out + s * kSegSyms; g[0] = 6; g[1] = 1; g[2] = 1; g[3] = 6; }
        const uint8_t* in = &il[(size_t)blk * 12 * kSegBytes];
        for (int e = 0; e < kNumEnc; e++) {
            uint8_t st = encState_[e];
            for (int k = 0; k < 828; k++) {
                const int dibit = (in[tm.byteIdx[e][k]] >> tm.shift[e][k]) & 3;
                const int idx = (st << 2) + dibit;
                st = tt.next[idx];
                out[tm.symPos[e][k]] = tt.out[idx];
            }
            encState_[e] = st;
        }
    }
    memcpy(last12_, symbols + (size_t)(kFieldSegs - 1) * kSegSyms + kSegSyms - 12, 12);
}

// ---------------------------------------------------------------- field decoder
FieldDecoder::FieldDecoder() { reset(); }

void FieldDecoder::reset() {
    dil_.reset();
    rnd_.reset();
    warm_ = kDelaySegs;
}

namespace {
// The twelve trellis encoders of a field are decoded side by side: every step of the Viterbi algorithm is the same for all of them, so
// the state metrics are kept as [state][encoder] and the loops over the encoders vectorise. Squared distance, incoming transitions of
// every state: source state, input dibit and the symbol sent (every state has exactly four).
struct VitTab {
    uint8_t from[8][4], in[8][4], sym[8][4];
    constexpr VitTab() : from(), in(), sym() {
        constexpr uint8_t next[32] = {0, 1, 4, 5, 2, 3, 6, 7, 1, 0, 5, 4, 3, 2, 7, 6, 4, 5, 0, 1, 6, 7, 2, 3, 5, 4, 1, 0, 7, 6, 3, 2};
        constexpr uint8_t out[32] = {0, 2, 4, 6, 1, 3, 5, 7, 0, 2, 4, 6, 1, 3, 5, 7, 4, 6, 0, 2, 5, 7, 1, 3, 4, 6, 0, 2, 5, 7, 1, 3};
        int cnt[8] = {};
        for (int s = 0; s < 8; s++) for (int u = 0; u < 4; u++) {
            const int ns = next[s * 4 + u];
            from[ns][cnt[ns]] = (uint8_t)s; in[ns][cnt[ns]] = (uint8_t)u; sym[ns][cnt[ns]] = out[s * 4 + u];
            cnt[ns]++;
        }
    }
};
constexpr VitTab kVit;

constexpr int kLanes = kNumEnc;

// xt[t * kLanes + l]: the level of encoder l at step t. dec[(t * 8 + ns) * kLanes + l]: the winning incoming transition. fin[l]: best end state.
void viterbiLanes(const float* xt, int n, uint8_t* dec, int* fin) {
    alignas(16) float pm[8][kLanes] = {}, nm[8][kLanes];
    for (int t = 0; t < n; t++) {
        const float* xv = xt + (size_t)t * kLanes;
        alignas(16) float d[8][kLanes];
        for (int s = 0; s < 8; s++) { const float lv = (float)(2 * s - 7); for (int l = 0; l < kLanes; l++) { const float v = xv[l] - lv; d[s][l] = v * v; } }
        uint8_t* dp = dec + (size_t)t * 8 * kLanes;
        alignas(16) float mn[kLanes];
        for (int l = 0; l < kLanes; l++) mn[l] = 1e30f;
#if defined(__clang__)
#pragma clang loop unroll(full)
#elif defined(__GNUC__)
#pragma GCC unroll 8
#endif
        for (int ns = 0; ns < 8; ns++) {
            const float* p0 = pm[kVit.from[ns][0]]; const float* p1 = pm[kVit.from[ns][1]]; const float* p2 = pm[kVit.from[ns][2]]; const float* p3 = pm[kVit.from[ns][3]];
            const float* d0 = d[kVit.sym[ns][0]]; const float* d1 = d[kVit.sym[ns][1]]; const float* d2 = d[kVit.sym[ns][2]]; const float* d3 = d[kVit.sym[ns][3]];
            for (int l = 0; l < kLanes; l++) {
                const float m0 = p0[l] + d0[l], m1 = p1[l] + d1[l], m2 = p2[l] + d2[l], m3 = p3[l] + d3[l];
                const bool b01 = m1 < m0, b23 = m3 < m2;
                const float a = b01 ? m1 : m0, c = b23 ? m3 : m2;
                const bool cw = c < a;
                nm[ns][l] = cw ? c : a;
                dp[ns * kLanes + l] = (uint8_t)(cw ? 2 + (int)b23 : (int)b01);
                mn[l] = nm[ns][l] < mn[l] ? nm[ns][l] : mn[l];
            }
        }
        for (int s = 0; s < 8; s++) for (int l = 0; l < kLanes; l++) pm[s][l] = nm[s][l] - mn[l];
    }
    for (int l = 0; l < kLanes; l++) {
        int state = 0;
        for (int s = 1; s < 8; s++) if (pm[s][l] < pm[state][l]) state = s;
        fin[l] = state;
    }
}
}

int FieldDecoder::decode(const float* levels, uint8_t* out, FieldStats* st, uint8_t* symbolsOut) {
    const TrellisMap& tm = trellisMap();
    std::vector<uint8_t> bytes((size_t)kDataSegs * kSegBytes, 0);
    const int per = 26 * 828;
    static thread_local std::vector<float> xt;
    static thread_local std::vector<uint8_t> decb;
    xt.resize((size_t)per * kLanes);
    decb.resize((size_t)per * 8 * kLanes);
    double dist = 0; long cntSym = 0;
    for (int blk = 0; blk < 26; blk++)
        for (int e = 0; e < kNumEnc; e++) {
            const float* src = levels + (size_t)(1 + blk * 12) * kSegSyms;
            float* dst = xt.data() + (size_t)blk * 828 * kLanes + e;
            const int* sp = tm.symPos[e];
            for (int k = 0; k < 828; k++) dst[(size_t)k * kLanes] = src[sp[k]];
        }
    int state[kLanes];
    viterbiLanes(xt.data(), per, decb.data(), state);
    for (int t = per; t-- > 0;) {
        const int blk = t / 828, k = t % 828;
        const uint8_t* dp = decb.data() + (size_t)t * 8 * kLanes;
        for (int e = 0; e < kNumEnc; e++) {
            const int ns = state[e];
            const int j = dp[ns * kLanes + e];
            const uint8_t dibit = kVit.in[ns][j];
            if (symbolsOut) symbolsOut[(size_t)(1 + blk * 12) * kSegSyms + tm.symPos[e][k]] = kVit.sym[ns][j];
            uint8_t& b = bytes[(size_t)blk * 12 * kSegBytes + tm.byteIdx[e][k]];
            b = (uint8_t)((b & ~(3 << tm.shift[e][k])) | (dibit << tm.shift[e][k]));
            state[e] = kVit.from[ns][j];
        }
    }
    (void)dist; (void)cntSym;
    dil_.syncCommutator();
    std::vector<uint8_t> de(bytes.size());
    dil_.process(bytes.data(), de.data(), bytes.size());
    int written = 0;
    for (int j = 0; j < kDataSegs; j++) {
        if (warm_ > 0) { warm_--; continue; }
        uint8_t blk[kSegBytes];
        memcpy(blk, &de[(size_t)j * kSegBytes], kSegBytes);
        const int r = rsDecode(blk);
        if (st) { st->segments++; if (r == 0) st->rsClean++; else if (r > 0) { st->rsCorrected++; st->bytesCorrected += r; } else st->rsFailed++; }
        uint8_t m[187];
        memcpy(m, blk, 187);
        if (j == kDelaySegs) rnd_.reset();   // the packet that was first in its field
        rnd_.apply(m, 187);
        uint8_t* o = out + (size_t)written * kTsBytes;
        o[0] = 0x47;
        memcpy(o + 1, m, 187);
        if (r < 0) o[1] |= 0x80;
        written++;
    }
    return written;
}

} // namespace atsc
} // namespace dect2
