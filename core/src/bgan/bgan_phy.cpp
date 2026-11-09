// BGAN forward bearers, physical layer (bgan_phy.h). ETSI TS 102 744-2-1.
#include "dect2/bgan/bgan_phy.h"
#include "dect2/common/inflate.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace dect2::bgan {

int bitsPerSymbol(Mod m) { return m == Mod::Q4 ? 2 : m == Mod::Q16 ? 4 : m == Mod::Q32 ? 5 : 6; }

namespace {

struct BganTableIndex { int family, subtype, N, symbols, bps, offset; };
#include "bgan_tables_data.inc"

struct Tables {
    std::vector<uint16_t> perm, map;
    std::vector<Table> t;
    Table* grid[12][kSubtypes] = {};
    Tables() {
        std::vector<uint8_t> raw;
        if (!inflateRaw(kBganBlob, sizeof kBganBlob, raw, 64u << 20) || raw.size() != kBganBlobRaw) return;
        const size_t n = sizeof kBganIndex / sizeof kBganIndex[0];
        size_t permTotal = 0, mapTotal = 0;
        for (size_t i = 0; i < n; i++) { permTotal += kBganIndex[i].N; mapTotal += size_t(kBganIndex[i].symbols) * kBganIndex[i].bps; }
        perm.resize(permTotal); map.resize(mapTotal);
        t.reserve(n);
        size_t pp = 0, pm = 0;
        for (size_t i = 0; i < n; i++) {
            const BganTableIndex& e = kBganIndex[i];
            size_t o = e.offset;
            int prev = 0;
            Table tb; tb.family = e.family; tb.subtype = e.subtype; tb.N = e.N; tb.symbols = e.symbols; tb.bps = e.bps;
            tb.perm = perm.data() + pp; tb.map = map.data() + pm;
            for (int j = 0; j < e.N; j++, o += 2) { prev = (prev + (raw[o] | raw[o + 1] << 8)) % e.N; perm[pp++] = uint16_t(prev); }
            prev = 0;
            const int M = 3 * e.N;
            for (int j = 0; j < e.symbols * e.bps; j++, o += 2) { prev = (prev + (raw[o] | raw[o + 1] << 8)) % M; map[pm++] = uint16_t(prev); }
            t.push_back(tb);
            grid[e.family][e.subtype] = &t.back();
        }
    }
};
const Tables& tabs() { static const Tables k; return k; }

const float S2 = 0.70710678f;

// TS 102 744-2-1 Table 5.10, 5.2, 5.2.3 and the unique word figures
const Bearer kBearers[] = {
    //  name               fb  rate    roll  mod        mos   uw  ps  ds   teo   n  outer  dyn   fam f16  axis           zeroPlus
    { "F80T0.25Q-1B",      0,  8400,  0.25, Mod::Q4,    672,  40, 88,   6,  544,  1, false, false,  0, -1, S2,            true  },
    { "F80T1Q-1B",         4, 33600,  0.25, Mod::Q4,   2688,  40, 88,  29, 2560,  1, false, false, -1, -1, S2,            true  },
    { "F80T1Q-4B",         1, 33600,  0.25, Mod::Q4,   2688,  40, 88,  29,  640,  4, false, false,  1, -1, S2,            true  },
    { "F80T1X-4B",         2, 33600,  0.25, Mod::Q16,  2688,  40, 88,  29,  640,  4, false, false,  2, -1, 0.94868330f,   false },
    { "FR80T2.5X4/16-5B",  5, 84000,  0.13, Mod::Q4,   6720,  40, 90,  73, 1318,  5, true,  true,   4,  5, S2,            false },
    { "FR80T2.5X16-5B",    6, 84000,  0.13, Mod::Q16,  6720,  40, 90,  73, 1318,  5, true,  false,  5, -1, 0.94868330f,   false },
    { "FR80T2.5X32-6B",    7, 84000,  0.13, Mod::Q32,  6720,  40, 92,  71, 1098,  6, true,  false,  6, -1, 0.922f,        false },
    { "FR80T2.5X64-7B",    8, 84000,  0.13, Mod::Q64,  6720,  40, 93,  70,  941,  7, true,  false,  7, -1, 0.9386f,       false },
    { "F80T4.5X-8B",       3, 151200, 0.25, Mod::Q16, 12096,  40, 88, 136, 1496,  8, false, false,  3, -1, 0.94868330f,   false },
    { "FR80T5X4/16-9B",    9, 168000, 0.13, Mod::Q4,  13440,  40, 89, 149, 1479,  9, true,  true,   8,  9, S2,            false },
    { "FR80T5X16-9B",     10, 168000, 0.13, Mod::Q16, 13440,  40, 89, 149, 1479,  9, true,  false,  9, -1, 0.94868330f,   false },
    { "FR80T5X32-11B",    11, 168000, 0.13, Mod::Q32, 13440,  40, 90, 147, 1210, 11, true,  false, 10, -1, 0.922f,        false },
    { "FR80T5X64-13B",    12, 168000, 0.13, Mod::Q64, 13440,  40, 88, 151, 1024, 13, true,  false, 11, -1, 0.9386f,       false },
};

const uint64_t kUw[kSubtypes] = {
    0xE4564ADABDull, 0xBED8B3EAD2ull, 0xF2F5F496A6ull, 0xC91136428Aull, 0xF9A42BB1ABull, 0xD4E357299Cull, 0x4CB9D9D174ull, 0x6AAF7A6E4Eull,
    0xC240E96587ull, 0x514BB8BA62ull, 0xB5896CCDDFull, 0xA87B0DA6C9ull, 0x5A1A679D6Full, 0x61FEA54943ull, 0xA32AD281C4ull,
};

const char* const kSub[kSubtypes] = { "L8", "L7", "L6", "L5", "L4", "L3", "L2", "L1", "R", "H1", "H2", "H3", "H4", "H5", "H6" };

// Tables 5.3 to 5.6
struct Const {
    cf32 q4[4], q16[16], q32[32], q64[64];
    Const() {
        const float D = 2.f / std::sqrt(2.f);
        q4[0] = cf32(D / 2, D / 2); q4[1] = cf32(D / 2, -D / 2); q4[2] = cf32(-D / 2, D / 2); q4[3] = cf32(-D / 2, -D / 2);
        const float D16 = 2.f / std::sqrt(10.f);
        // b3 = sign of I (1 is +), b2 = magnitude of I (1 is 3D/2), b1 / b0 the same for Q
        for (int i = 0; i < 16; i++) {
            const int b3 = i >> 3 & 1, b2 = i >> 2 & 1, b1 = i >> 1 & 1, b0 = i & 1;
            q16[i] = cf32((b3 ? 1.f : -1.f) * (b2 ? 1.5f : 0.5f) * D16, (b1 ? 1.f : -1.f) * (b0 ? 1.5f : 0.5f) * D16);
        }
        const float D32 = 2.f / std::sqrt(20.f);
        static const signed char t32[32][2] = {
            {-1,1},{1,1},{-1,-1},{1,-1},{-1,3},{1,3},{-1,5},{1,5},{-1,-5},{1,-5},{-1,-3},{1,-3},{-3,3},{3,3},{-3,5},{3,5},
            {-3,1},{3,1},{-3,-1},{3,-1},{-5,1},{5,1},{-5,-1},{5,-1},{-3,-5},{3,-5},{-3,-3},{3,-3},{-5,3},{5,3},{-5,-3},{5,-3} };
        for (int i = 0; i < 32; i++) q32[i] = cf32(t32[i][0] * D32 / 2, t32[i][1] * D32 / 2);
        const float D64 = 2.f / std::sqrt(42.f);
        static const signed char a64[8] = { 3, 1, 5, 7, -3, -1, -5, -7 };
        for (int i = 0; i < 64; i++) q64[i] = cf32(a64[i >> 3 & 7] * D64 / 2, a64[i & 7] * D64 / 2);
    }
};
const Const& cst() { static const Const k; return k; }

} // namespace

const cf32* constellation(Mod m, int& points) {
    const Const& c = cst();
    switch (m) {
    case Mod::Q4: points = 4; return c.q4;
    case Mod::Q16: points = 16; return c.q16;
    case Mod::Q32: points = 32; return c.q32;
    default: points = 64; return c.q64;
    }
}

void softDemap(Mod m, cf32 y, float nvar, float* llr) {
    int n = 0;
    const cf32* c = constellation(m, n);
    const int bps = bitsPerSymbol(m);
    float m0[6], m1[6];
    for (int b = 0; b < bps; b++) { m0[b] = 1e30f; m1[b] = 1e30f; }
    for (int i = 0; i < n; i++) {
        const float d = std::norm(y - c[i]);
        for (int b = 0; b < bps; b++) {
            if (i >> (bps - 1 - b) & 1) m1[b] = std::min(m1[b], d);
            else m0[b] = std::min(m0[b], d);
        }
    }
    for (int b = 0; b < bps; b++) llr[b] = (m1[b] - m0[b]) / nvar;
}

int bearerCount() { return int(sizeof kBearers / sizeof kBearers[0]); }
const Bearer& bearer(int i) { return kBearers[i]; }
const char* subtypeName(int s) { return s >= 0 && s < kSubtypes ? kSub[s] : "?"; }
const uint64_t* uniqueWords() { return kUw; }
int bearerUwIndexFromName(const char* name) {
    for (int i = 0; i < bearerCount(); i++) if (!std::strcmp(kBearers[i].name, name)) return i;
    return -1;
}

const Table* table(int family, int subtype) {
    if (family < 0 || family >= 12 || subtype < 0 || subtype >= kSubtypes) return nullptr;
    return tabs().grid[family][subtype];
}
int tableCount() { return int(tabs().t.size()); }

FrameLayout frameLayout(const Bearer& b) {
    FrameLayout f;
    f.dataPos.reserve(size_t(b.teo) * b.blocks);
    f.pilotPos.reserve(b.ps);
    int pos = b.uw;
    for (int i = 0; i < b.ps; i++) {
        for (int k = 0; k < b.ds; k++) f.dataPos.push_back(pos++);
        f.pilotPos.push_back(pos++);
    }
    while (pos < b.mos) f.dataPos.push_back(pos++);
    return f;
}

void scramble(uint8_t* bits, int n) {
    uint32_t r = 0x6959;       // bit 14 = stage #15 ... bit 0 = stage #1
    for (int i = 0; i < n; i++) {
        const uint32_t fb = (r ^ (r >> 14)) & 1;
        bits[i] ^= uint8_t(fb);
        r = (r >> 1) | (fb << 14);
    }
}

namespace {

// one trellis step: state = T1 T2 T3 T4 (T1 is bit 3)
inline void step(int s, int u, int& next, int& p) {
    const int t1 = s >> 3 & 1, t2 = s >> 2 & 1, t3 = s >> 1 & 1, t4 = s & 1;
    const int w = u ^ t3 ^ t4;
    p = w ^ t1 ^ t2 ^ t4;
    next = (w << 3) | (t1 << 2) | (t2 << 1) | t3;
}
// Table 5.11: the flush bits as a function of the final state (written T1 T2 T3 T4, 1 = rightmost)
const int kFlush[16][4] = {
    {0,0,0,0},{1,0,0,0},{1,1,0,0},{0,1,0,0},{0,1,1,0},{1,1,1,0},{1,0,1,0},{0,0,1,0},
    {0,0,1,1},{1,0,1,1},{1,1,1,1},{0,1,1,1},{0,1,0,1},{1,1,0,1},{1,0,0,1},{0,0,0,1},
};

} // namespace

void encodeBlock(const Table& t, Mod m, const uint8_t* bytes, cf32* syms) {
    const int N = t.N, D = N - 4;
    std::vector<uint8_t> d(N), p(N), q(N), raw(3 * N);
    for (int i = 0; i < D; i++) d[i] = (bytes[i / 8] >> (7 - i % 8)) & 1;
    scramble(d.data(), D);
    int s = 0, nx, pb;
    for (int i = 0; i < D; i++) { step(s, d[i], nx, pb); p[i] = uint8_t(pb); s = nx; }
    for (int i = 0; i < 4; i++) {
        d[D + i] = uint8_t(kFlush[s][i]);
    }
    int fs = s;
    for (int i = 0; i < 4; i++) { step(fs, d[D + i], nx, pb); p[D + i] = uint8_t(pb); fs = nx; }
    int s2 = 0;
    for (int i = 0; i < N; i++) { step(s2, d[t.perm[i]], nx, pb); q[i] = uint8_t(pb); s2 = nx; }
    std::memcpy(raw.data(), d.data(), N); std::memcpy(raw.data() + N, p.data(), N); std::memcpy(raw.data() + 2 * N, q.data(), N);
    int points = 0;
    const cf32* c = constellation(m, points);
    for (int i = 0; i < t.symbols; i++) {
        int v = 0;
        for (int b = 0; b < t.bps; b++) v = v * 2 + raw[t.map[i * t.bps + b]];
        syms[i] = c[v];
    }
}

namespace {

// max-log-MAP over the 16-state trellis; systematic LLR `ls`, parity `lp`; extrinsic written to `ext`; terminated: the last state is 0
void mapDecode(int N, const float* ls, const float* lp, const float* apriori, std::vector<float>& a, std::vector<float>& b, float* ext, bool terminated) {
    a.assign(size_t(N + 1) * 16, -1e30f);
    b.assign(size_t(N + 1) * 16, -1e30f);
    a[0] = 0;
    static int nx[16][2], pb[16][2];
    static const bool init = [] { for (int s = 0; s < 16; s++) for (int u = 0; u < 2; u++) step(s, u, nx[s][u], pb[s][u]); return true; }();
    (void)init;
    for (int i = 0; i < N; i++) {
        const float sa = (ls[i] + apriori[i]) * 0.5f, pa = lp[i] * 0.5f;
        const float* ap = &a[size_t(i) * 16];
        float* an = &a[size_t(i + 1) * 16];
        for (int s = 0; s < 16; s++) {
            const float v = ap[s];
            if (v < -1e29f) continue;
            for (int u = 0; u < 2; u++) {
                const float g = (u ? -sa : sa) + (pb[s][u] ? -pa : pa);
                float& t = an[nx[s][u]];
                t = std::max(t, v + g);
            }
        }
    }
    if (terminated) b[size_t(N) * 16] = 0;
    else for (int s = 0; s < 16; s++) b[size_t(N) * 16 + s] = 0;
    for (int i = N - 1; i > 0; i--) {
        const float sa = (ls[i] + apriori[i]) * 0.5f, pa = lp[i] * 0.5f;
        const float* bn = &b[size_t(i + 1) * 16];
        float* bp = &b[size_t(i) * 16];
        for (int s = 0; s < 16; s++) {
            float best = -1e30f;
            for (int u = 0; u < 2; u++) {
                const float g = (u ? -sa : sa) + (pb[s][u] ? -pa : pa);
                best = std::max(best, g + bn[nx[s][u]]);
            }
            bp[s] = best;
        }
    }
    for (int i = 0; i < N; i++) {
        const float pa = lp[i] * 0.5f;
        const float* ap = &a[size_t(i) * 16];
        const float* bn = &b[size_t(i + 1) * 16];
        float m0 = -1e30f, m1 = -1e30f;
        for (int s = 0; s < 16; s++) {
            if (ap[s] < -1e29f) continue;
            for (int u = 0; u < 2; u++) {
                const float g = (pb[s][u] ? -pa : pa);
                const float v = ap[s] + g + bn[nx[s][u]];
                if (u) m1 = std::max(m1, v); else m0 = std::max(m0, v);
            }
        }
        ext[i] = m0 - m1;
    }
}

} // namespace

int TurboDecoder::decode(const Table& t, const float* llr, std::vector<uint8_t>& out, int maxIter, const std::function<bool(const std::vector<uint8_t>&)>& ok) {
    const int N = t.N;
    ld_.assign(N, 0); lp_.assign(N, 0); lq_.assign(N, 0);
    for (int i = 0; i < t.symbols * t.bps; i++) {
        const int m = t.map[i];
        if (m < N) ld_[m] += llr[i];
        else if (m < 2 * N) lp_[m - N] += llr[i];
        else lq_[m - 2 * N] += llr[i];
    }
    std::vector<float> ls2(N), ap1(N, 0), ap2(N, 0);
    ext1_.resize(N); ext2_.resize(N);
    for (int i = 0; i < N; i++) ls2[i] = ld_[t.perm[i]];
    int iter = 0;
    out.resize(t.dataBytes());
    for (iter = 1; iter <= maxIter; iter++) {
        mapDecode(N, ld_.data(), lp_.data(), ap1.data(), a_, b_, ext1_.data(), true);
        for (int i = 0; i < N; i++) ext1_[i] *= 0.72f;
        for (int i = 0; i < N; i++) ap2[i] = ext1_[t.perm[i]];
        mapDecode(N, ls2.data(), lq_.data(), ap2.data(), a_, b_, ext2_.data(), false);
        for (int i = 0; i < N; i++) ext2_[i] *= 0.72f;
        for (int i = 0; i < N; i++) ap1[t.perm[i]] = ext2_[i];
        // decide
        hard_.assign(N, 0);
        for (int i = 0; i < N; i++) hard_[i] = uint8_t((ld_[i] + ap1[i] + ext1_[i]) < 0);
        std::vector<uint8_t> desc(hard_.begin(), hard_.end() - 4);
        scramble(desc.data(), int(desc.size()));
        std::memset(out.data(), 0, out.size());
        for (size_t i = 0; i + 1 <= desc.size() && i / 8 < out.size(); i++) out[i / 8] |= desc[i] << (7 - i % 8);
        if (ok && ok(out)) break;
    }
    return std::min(iter, maxIter);
}

} // namespace dect2::bgan
