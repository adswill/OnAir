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

void rsEncode(const uint8_t* in, uint8_t* out) {
    static const RsGen gen;
    const Gf& f = gf();
    uint8_t par[kRoots] = {};
    for (int i = 0; i < 187; i++) {
        out[i] = in[i];
        const uint8_t fb = in[i] ^ par[0];
        memmove(par, par + 1, kRoots - 1);
        par[kRoots - 1] = 0;
        if (fb) for (int j = 0; j < kRoots; j++) par[j] ^= f.mul(fb, gen.g[j + 1]);
    }
    memcpy(out + 187, par, kRoots);
}

int rsDecode(uint8_t* r) {
    const Gf& f = gf();
    uint8_t S[kRoots];
    bool any = false;
    for (int i = 0; i < kRoots; i++) {
        uint8_t s = 0;
        for (int j = 0; j < kN; j++) s = f.mul(s, f.exp[i]) ^ r[j];
        S[i] = s;
        any |= s != 0;
    }
    if (!any) return 0;
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
    for (int i = 0; i < kRoots; i++) { uint8_t s = 0; for (int j = 0; j < kN; j++) s = f.mul(s, f.exp[i]) ^ r[j]; if (s) return -1; }
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
// Viterbi over one encoder's symbol sequence (squared distance), returns the decoded dibits
float viterbiRun(const float* x, int n, uint8_t* dibits, uint8_t* symbols = nullptr) {
    // incoming transitions of every state: source state, input dibit and the symbol sent
    struct Tab {
        uint8_t from[8][4], in[8][4], sym[8][4];
        Tab() {
            const TrellisTables& t = trellis();
            int cnt[8] = {};
            for (int s = 0; s < 8; s++) for (int u = 0; u < 4; u++) {
                const int ns = t.next[s * 4 + u];
                from[ns][cnt[ns]] = (uint8_t)s; in[ns][cnt[ns]] = (uint8_t)u; sym[ns][cnt[ns]] = t.out[s * 4 + u];
                cnt[ns]++;
            }
        }
    };
    static const Tab tab;   // every state has exactly four incoming transitions
    std::vector<uint8_t> dec((size_t)n * 8);
    float pm[8] = {}, nm[8];
    for (int t = 0; t < n; t++) {
        float d[8];
        const float xv = x[t];
        for (int s = 0; s < 8; s++) { const float v = xv - (float)(2 * s - 7); d[s] = v * v; }
        uint8_t* dp = &dec[(size_t)t * 8];
        float mn = 1e30f;
        for (int ns = 0; ns < 8; ns++) {
            float m0 = pm[tab.from[ns][0]] + d[tab.sym[ns][0]];
            float m1 = pm[tab.from[ns][1]] + d[tab.sym[ns][1]];
            float m2 = pm[tab.from[ns][2]] + d[tab.sym[ns][2]];
            float m3 = pm[tab.from[ns][3]] + d[tab.sym[ns][3]];
            uint8_t b01 = m1 < m0, b23 = m3 < m2;
            const float a = b01 ? m1 : m0, c = b23 ? m3 : m2;
            const uint8_t bi = c < a ? (uint8_t)(2 + b23) : b01;
            const float best = c < a ? c : a;
            nm[ns] = best;
            dp[ns] = bi;
            mn = best < mn ? best : mn;
        }
        for (int s = 0; s < 8; s++) pm[s] = nm[s] - mn;
    }
    int state = 0;
    for (int s = 1; s < 8; s++) if (pm[s] < pm[state]) state = s;
    for (int t = n; t-- > 0;) {
        const int k = dec[(size_t)t * 8 + state];
        dibits[t] = tab.in[state][k];
        if (symbols) symbols[t] = tab.sym[state][k];
        state = tab.from[state][k];
    }
    return 0;
}
}

int FieldDecoder::decode(const float* levels, uint8_t* out, FieldStats* st, uint8_t* symbolsOut) {
    const TrellisMap& tm = trellisMap();
    std::vector<uint8_t> bytes((size_t)kDataSegs * kSegBytes, 0);
    const int per = 26 * 828;
    std::vector<float> x(per);
    std::vector<uint8_t> dib(per), psym(symbolsOut ? per : 0);
    double dist = 0; long cntSym = 0;
    for (int e = 0; e < kNumEnc; e++) {
        for (int blk = 0; blk < 26; blk++)
            for (int k = 0; k < 828; k++)
                x[blk * 828 + k] = levels[(size_t)(1 + blk * 12) * kSegSyms + tm.symPos[e][k]];
        viterbiRun(x.data(), per, dib.data(), symbolsOut ? psym.data() : nullptr);
        if (symbolsOut)
            for (int blk = 0; blk < 26; blk++)
                for (int k = 0; k < 828; k++) symbolsOut[(size_t)(1 + blk * 12) * kSegSyms + tm.symPos[e][k]] = psym[blk * 828 + k];
        for (int blk = 0; blk < 26; blk++)
            for (int k = 0; k < 828; k++) {
                uint8_t& b = bytes[(size_t)blk * 12 * kSegBytes + tm.byteIdx[e][k]];
                b = (uint8_t)((b & ~(3 << tm.shift[e][k])) | (dib[blk * 828 + k] << tm.shift[e][k]));
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
