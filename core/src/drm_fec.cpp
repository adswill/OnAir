// DRM channel coding: CRC, energy dispersal, interleavers, constellations, multilevel coding with the punctured convolutional code.
#include "dect2/drm_fec.h"
#include "drm_internal.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

namespace dect2 { namespace drm {

// ---------------------------------------------------------------- CRC (Annex D)

namespace {
uint32_t crcBits(const uint8_t* bits, size_t n, uint32_t poly, int width) {
    const uint32_t mask = (width == 32) ? 0xFFFFFFFFu : ((1u << width) - 1);
    uint32_t reg = mask;                                    // all ones at the start
    for (size_t i = 0; i < n; i++) {
        const uint32_t fb = ((reg >> (width - 1)) & 1u) ^ (bits[i] & 1u);
        reg = (reg << 1) & mask;
        if (fb) reg ^= poly;
    }
    return reg ^ mask;                                      // 1's complement before transmission
}
}

uint32_t crc8(const uint8_t* bits, size_t n) { return crcBits(bits, n, 0x1D, 8); }
uint32_t crc16(const uint8_t* bits, size_t n) { return crcBits(bits, n, 0x1021, 16); }
uint32_t crc8Bytes(const uint8_t* bytes, size_t n) {
    std::vector<uint8_t> b(n * 8);
    for (size_t i = 0; i < n; i++) for (int j = 0; j < 8; j++) b[i * 8 + j] = (bytes[i] >> (7 - j)) & 1;
    return crc8(b.data(), b.size());
}
uint32_t crc16Bytes(const uint8_t* bytes, size_t n) {
    std::vector<uint8_t> b(n * 8);
    for (size_t i = 0; i < n; i++) for (int j = 0; j < 8; j++) b[i * 8 + j] = (bytes[i] >> (7 - j)) & 1;
    return crc16(b.data(), b.size());
}

// ---------------------------------------------------------------- energy dispersal (clause 7.2.2): P(X) = X^9 + X^5 + 1, all ones at the start

void prbs(uint8_t* out, size_t n) {
    uint32_t r = 0x1FF;                                     // bit i = stage i; stage 0 is the newest
    for (size_t i = 0; i < n; i++) {
        const uint32_t o = ((r >> 8) ^ (r >> 4)) & 1u;
        out[i] = (uint8_t)o;
        r = ((r << 1) | o) & 0x1FF;
    }
}

// ---------------------------------------------------------------- interleaver permutation (clauses 7.3.3.0, 7.6)

const std::vector<int>& interleavePerm(int xin, int t) {
    static std::mutex mu;
    static std::map<std::pair<int, int>, std::vector<int>> cache;
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find({xin, t});
    if (it != cache.end()) return it->second;
    std::vector<int> pi((size_t)std::max(xin, 1), 0);
    int s = 1;
    while (s < xin) s <<= 1;
    const int q = s / 4 - 1;
    for (int i = 1; i < xin; i++) {
        long v = ((long)t * pi[(size_t)i - 1] + q) % s;
        while (v >= xin) v = ((long)t * v + q) % s;
        pi[(size_t)i] = (int)v;
    }
    return cache.emplace(std::make_pair(xin, t), std::move(pi)).first->second;
}

// ---------------------------------------------------------------- constellations (clause 7.4, figures 26, 29, 30)

float qamNorm(int levels) { return levels == 1 ? 0.70710678f : levels == 2 ? 0.31622777f : 0.15430335f; }

int railValue(int levels, int b0, int b1, int b2) {
    const int idx = b0 + 2 * b1 + 4 * b2;                   // the figures give 7 - 2 idx (64-QAM), 3 - 2 idx (16-QAM), 1 - 2 idx (4-QAM)
    return ((1 << levels) - 1) - 2 * idx;
}

cf32 qamMap(int levels, const uint8_t* b) {
    const float a = qamNorm(levels);
    const int re = railValue(levels, b[0], levels > 1 ? b[1] : 0, levels > 2 ? b[2] : 0);
    const int im = railValue(levels, b[levels], levels > 1 ? b[levels + 1] : 0, levels > 2 ? b[levels + 2] : 0);
    return cf32(a * (float)re, a * (float)im);
}

void qamDemapHard(int levels, cf32 z, uint8_t* bits) {
    const float a = qamNorm(levels);
    const float rails[2] = {z.real() / a, z.imag() / a};
    const int n = 1 << levels;
    for (int r = 0; r < 2; r++) {
        int best = 0; float bd = 1e30f;
        for (int idx = 0; idx < n; idx++) {
            const float d = std::fabs(rails[r] - (float)((n - 1) - 2 * idx));
            if (d < bd) { bd = d; best = idx; }
        }
        for (int l = 0; l < levels; l++) bits[r * levels + l] = (uint8_t)((best >> l) & 1);
    }
}

// ---------------------------------------------------------------- convolutional code

namespace {
uint32_t genMask(int octal) {                               // tap t of the register <-> bit (6 - t) of the polynomial
    uint32_t m = 0;
    for (int t = 0; t < 7; t++) if ((octal >> (6 - t)) & 1) m |= 1u << t;
    return m;
}
const uint32_t kGen[3] = {genMask(0133), genMask(0171), genMask(0145)};   // outputs 3, 4, 5 repeat 0, 1, 2

inline int parity(uint32_t v) { return __builtin_parity(v); }

struct Trellis {
    uint8_t br[64][2];       // for next state ns and predecessor choice b: index 0..7 of the three output bits (o0 | o1 << 1 | o2 << 2)
    Trellis() {
        for (int ns = 0; ns < 64; ns++)
            for (int b = 0; b < 2; b++) {
                const int s = (ns >> 1) | (b << 5), u = ns & 1;
                const uint32_t w = (uint32_t)u | ((uint32_t)s << 1);
                br[ns][b] = (uint8_t)(parity(w & kGen[0]) | (parity(w & kGen[1]) << 1) | (parity(w & kGen[2]) << 2));
            }
    }
};
const Trellis& trellis() { static const Trellis t; return t; }
}

void convEncode(const uint8_t* in, int n, uint8_t* out) {
    uint32_t s = 0;
    for (int i = 0; i < n + 6; i++) {
        const uint32_t u = i < n ? (in[i] & 1u) : 0u;
        const uint32_t w = u | (s << 1);
        for (int j = 0; j < 6; j++) out[(size_t)i * 6 + (size_t)j] = (uint8_t)parity(w & kGen[j % 3]);
        s = ((s << 1) | u) & 63u;
    }
}

void viterbiDecode(const float* soft, int n, uint8_t* out) {
    const Trellis& T = trellis();
    const int steps = n + 6;
    std::vector<uint64_t> dec((size_t)steps);
    float pm[64], npm[64];
    for (int i = 0; i < 64; i++) pm[i] = -1e30f;
    pm[0] = 0;
    for (int i = 0; i < steps; i++) {
        const float* sf = soft + (size_t)i * 6;
        const float s0 = sf[0] + sf[3], s1 = sf[1] + sf[4], s2 = sf[2] + sf[5];
        float bm[8];
        for (int o = 0; o < 8; o++) bm[o] = ((o & 1) ? -s0 : s0) + ((o & 2) ? -s1 : s1) + ((o & 4) ? -s2 : s2);
        uint64_t d = 0;
        float top = -1e30f;
        for (int ns = 0; ns < 64; ns++) {
            const float c0 = pm[ns >> 1] + bm[T.br[ns][0]];
            const float c1 = pm[(ns >> 1) | 32] + bm[T.br[ns][1]];
            if (c1 > c0) { npm[ns] = c1; d |= (uint64_t)1 << ns; } else npm[ns] = c0;
            top = std::max(top, npm[ns]);
        }
        dec[(size_t)i] = d;
        for (int ns = 0; ns < 64; ns++) pm[ns] = npm[ns] - top;   // keeps the metrics near zero
    }
    uint32_t st = 0;
    for (int i = steps - 1; i >= 0; i--) {
        if (i < n) out[i] = (uint8_t)(st & 1u);
        const uint32_t b = (uint32_t)((dec[(size_t)i] >> st) & 1u);
        st = (st >> 1) | (b << 5);
    }
}

// ---------------------------------------------------------------- multilevel coding

int mlcBitsA(int n1, int rx, int ry) { return n1 > 0 ? (2 * n1 * rx) / ry : 0; }
int mlcBitsB(int n2, int rx, int ry) { return rx * ((2 * n2 - 12) / ry); }

namespace {
const PunctPattern* findPattern(int rx, int ry) {
    for (const auto& p : kPunct) if (p.rx == rx && p.ry == ry) return &p;
    return nullptr;
}
int bitIntlT(int levels, int level) {                       // clause 7.3.3.0: multiplier of the bit interleaver, 0 = this level is not interleaved
    if (levels == 3) return level == 0 ? 0 : level == 1 ? 13 : 21;
    if (levels == 2) return level == 0 ? 13 : 21;
    return 21;
}
}

struct MlcCode::Impl {
    int levels = 1, nCells = 0, n1 = 0, n2 = 0;
    int m1[3] = {}, m2[3] = {};                 // information bits per level and part
    int steps[3] = {};                          // encoder steps per level (information + 6 tail)
    std::vector<uint32_t> map[3];               // transmitted position -> index into the mother code output
    const std::vector<int>* permA[3] = {};      // bit interleaver of each level and part (nullptr: none)
    const std::vector<int>* permB[3] = {};
};

MlcCode::MlcCode(const MlcParams& p) : p_(p), d_(std::make_shared<Impl>()) {
    Impl& d = *d_;
    d.levels = p.levels; d.n1 = p.n1; d.n2 = p.n2; d.nCells = p.n1 + p.n2;
    for (int l = 0; l < p.levels; l++) {
        const PunctPattern* pa = p.n1 > 0 ? findPattern(p.rxA[l], p.ryA[l]) : nullptr;
        const PunctPattern* pb = findPattern(p.rxB[l], p.ryB[l]);
        assert(pb && (p.n1 == 0 || pa));
        if (p.fac) {
            d.m1[l] = 0;
            d.m2[l] = (2 * p.n2 * p.rxB[l]) / p.ryB[l] - 6;
        } else {
            d.m1[l] = mlcBitsA(p.n1, p.rxA[l], p.ryA[l]);
            d.m2[l] = mlcBitsB(p.n2, p.rxB[l], p.ryB[l]);
        }
        const int M = d.m1[l] + d.m2[l];
        d.steps[l] = M + 6;
        std::vector<uint32_t>& mp = d.map[l];
        mp.clear();
        for (int i = 0; i < d.m1[l]; i++) {
            const int c = i % pa->cols;
            for (int j = 0; j < 6; j++) if (pa->row[j][c]) mp.push_back((uint32_t)(i * 6 + j));
        }
        for (int i = d.m1[l]; i < M; i++) {
            const int c = (i - d.m1[l]) % pb->cols;
            for (int j = 0; j < 6; j++) if (pb->row[j][c]) mp.push_back((uint32_t)(i * 6 + j));
        }
        if (p.fac) {                                           // the FAC punctures the tail with the data pattern
            for (int i = M; i < M + 6; i++) {
                const int c = (i - d.m1[l]) % pb->cols;
                for (int j = 0; j < 6; j++) if (pb->row[j][c]) mp.push_back((uint32_t)(i * 6 + j));
            }
        } else {                                               // Table 28
            const int r = (2 * p.n2 - 12) - p.ryB[l] * ((2 * p.n2 - 12) / p.ryB[l]);
            for (int t = 0; t < 6; t++)
                for (int j = 0; j < 6; j++) if (kTailPunct[r][j][t]) mp.push_back((uint32_t)((M + t) * 6 + j));
        }
        assert((int)mp.size() == 2 * d.nCells);
        const int t = bitIntlT(p.levels, l);
        d.permA[l] = (t && p.n1 > 0) ? &interleavePerm(2 * p.n1, t) : nullptr;
        d.permB[l] = t ? &interleavePerm(2 * p.n2, t) : nullptr;
        l1_ += d.m1[l]; l2_ += d.m2[l];
    }
}

void MlcCode::encode(const uint8_t* u, cf32* cells) const {
    const Impl& d = *d_;
    const int N = d.nCells;
    std::vector<uint8_t> y[3];
    std::vector<uint8_t> x, c, v((size_t)2 * N);
    const uint8_t* a = u;                                      // part A bits: levels in order
    const uint8_t* b = u + l1_;                                // part B bits
    for (int l = 0; l < d.levels; l++) {
        x.assign(a, a + d.m1[l]); a += d.m1[l];
        x.insert(x.end(), b, b + d.m2[l]); b += d.m2[l];
        c.assign((size_t)d.steps[l] * 6, 0);
        convEncode(x.data(), (int)x.size(), c.data());
        for (int i = 0; i < 2 * N; i++) v[(size_t)i] = c[d.map[l][(size_t)i]];
        y[l].assign((size_t)2 * N, 0);
        for (int i = 0; i < 2 * d.n1; i++) y[l][(size_t)i] = d.permA[l] ? v[(size_t)(*d.permA[l])[(size_t)i]] : v[(size_t)i];
        for (int i = 0; i < 2 * d.n2; i++) y[l][(size_t)(2 * d.n1 + i)] = d.permB[l] ? v[(size_t)(2 * d.n1 + (*d.permB[l])[(size_t)i])] : v[(size_t)(2 * d.n1 + i)];
    }
    uint8_t bits[6];
    for (int i = 0; i < N; i++) {
        for (int l = 0; l < d.levels; l++) { bits[l] = y[l][(size_t)(2 * i)]; bits[d.levels + l] = y[l][(size_t)(2 * i + 1)]; }
        cells[i] = qamMap(d.levels, bits);
    }
}

void MlcCode::decode(const cf32* z, const float* w, uint8_t* u, int iterations) const {
    const Impl& d = *d_;
    const int N = d.nCells, L = d.levels, nPts = 1 << L;
    const float a = qamNorm(L), a2 = a * a;
    std::vector<uint8_t> yHard[3];
    bool known[3] = {false, false, false};
    std::vector<float> llr((size_t)2 * N), v((size_t)2 * N);
    std::vector<uint8_t> xs[3];
    std::vector<float> soft;
    std::vector<uint8_t> c, hv((size_t)2 * N);
    for (int it = 0; it < std::max(1, iterations); it++) {
        for (int l = 0; l < L; l++) {
            // soft values of the bits of level l, given the decisions that are known
            uint32_t kmask = 0;
            for (int q = 0; q < L; q++) if (q != l && known[q] && (it > 0 || q < l)) kmask |= 1u << q;
            for (int i = 0; i < N; i++) {
                const float rail[2] = {z[i].real() / a, z[i].imag() / a};
                for (int r = 0; r < 2; r++) {
                    uint32_t kv = 0;
                    for (int q = 0; q < L; q++) if (kmask & (1u << q)) kv |= (uint32_t)yHard[q][(size_t)(2 * i + r)] << q;
                    float d0 = 1e30f, d1 = 1e30f;
                    for (int idx = 0; idx < nPts; idx++) {
                        if ((((uint32_t)idx ^ kv) & kmask) != 0) continue;
                        const float e = rail[r] - (float)((nPts - 1) - 2 * idx);
                        const float dd = e * e;
                        if ((idx >> l) & 1) d1 = std::min(d1, dd); else d0 = std::min(d0, dd);
                    }
                    llr[(size_t)(2 * i + r)] = w[i] * a2 * (d1 - d0);
                }
            }
            // deinterleave and depuncture
            for (int i = 0; i < 2 * d.n1; i++) v[(size_t)(d.permA[l] ? (*d.permA[l])[(size_t)i] : i)] = llr[(size_t)i];
            for (int i = 0; i < 2 * d.n2; i++) v[(size_t)(2 * d.n1 + (d.permB[l] ? (*d.permB[l])[(size_t)i] : i))] = llr[(size_t)(2 * d.n1 + i)];
            soft.assign((size_t)d.steps[l] * 6, 0.f);
            for (int i = 0; i < 2 * N; i++) soft[d.map[l][(size_t)i]] = v[(size_t)i];
            xs[l].assign((size_t)(d.m1[l] + d.m2[l]), 0);
            viterbiDecode(soft.data(), d.m1[l] + d.m2[l], xs[l].data());
            // re-encode: the hard decisions of this level in the transmitted (interleaved) order
            c.assign((size_t)d.steps[l] * 6, 0);
            convEncode(xs[l].data(), (int)xs[l].size(), c.data());
            for (int i = 0; i < 2 * N; i++) hv[(size_t)i] = c[d.map[l][(size_t)i]];
            yHard[l].assign((size_t)2 * N, 0);
            for (int i = 0; i < 2 * d.n1; i++) yHard[l][(size_t)i] = d.permA[l] ? hv[(size_t)(*d.permA[l])[(size_t)i]] : hv[(size_t)i];
            for (int i = 0; i < 2 * d.n2; i++) yHard[l][(size_t)(2 * d.n1 + i)] = d.permB[l] ? hv[(size_t)(2 * d.n1 + (*d.permB[l])[(size_t)i])] : hv[(size_t)(2 * d.n1 + i)];
            known[l] = true;
        }
    }
    uint8_t* pa = u;
    uint8_t* pb = u + l1_;
    for (int l = 0; l < L; l++) {
        std::memcpy(pa, xs[l].data(), (size_t)d.m1[l]); pa += d.m1[l];
        std::memcpy(pb, xs[l].data() + d.m1[l], (size_t)d.m2[l]); pb += d.m2[l];
    }
}

}} // namespace dect2::drm
