// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The DVB-T2 receiver's LDPC decoder: the layered normalised min-sum of LdpcCode::decodeFast (core/src/dsp/ldpc.cpp), with the same
// arithmetic in the same order (so the same decisions and iteration counts, bit for bit), minus what it cost around the arithmetic:
//   - the work buffers live on with the thread (the message store alone is over a megabyte for a 64800-bit code: allocated, page-faulted in
//     and zeroed for every block before);
//   - the first iteration does not read the messages (they are all zero then), so nothing has to be cleared;
//   - the parity check after each iteration runs on sign bytes, 360 checks of a layer at a time, instead of one check and one variable at a time.
// On a good signal a block takes one or two iterations and those costs were most of its time. tests: test_fec (t2), bench_dvb.
#include "dect2/t2/t2fec.h"
#include "dect2/dsp/simd.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
#include <arm_neon.h>
#endif
#if defined(DECT2_X86_DISPATCH)
#include <immintrin.h>
#endif

namespace dect2 {

#if defined(DECT2_X86_DISPATCH)
#define DECT2_NOT_X86 false
#else
#define DECT2_NOT_X86 true
#endif
namespace {
constexpr int Z = 360;
constexpr float kBig = 1e4f;

#if !(defined(__ARM_NEON) && !defined(DECT2_NO_SIMD))
// the kernels of ldpc.cpp (branch-free, vectorised by the compiler; AVX2 clone on x86-64)
DECT2_SIMD_BODY void accumulate(const float* __restrict Lj, float* __restrict min1, float* __restrict min2, int* __restrict idx1,
                                uint32_t* __restrict sg, int j) {
    for (int m = 0; m < Z; m++) {
        const float x = Lj[m], a = std::fabs(x);
        uint32_t sb; std::memcpy(&sb, &x, 4);
        sg[m] ^= sb & 0x80000000u;
        const float m1 = min1[m], m2 = min2[m];
        const bool lt = a < m1;
        min2[m] = lt ? m1 : (a < m2 ? a : m2);
        idx1[m] = lt ? j : idx1[m];
        min1[m] = lt ? a : m1;
    }
}
template <bool kFirst>
DECT2_SIMD_BODY void apply(float* __restrict Lj, float* __restrict mj, const float* __restrict min1, const float* __restrict min2,
                           const int* __restrict idx1, const uint32_t* __restrict sg, int j, float alpha) {
    for (int m = 0; m < Z; m++) {
        const float mag = (idx1[m] == j ? min2[m] : min1[m]) * alpha;
        uint32_t sb; std::memcpy(&sb, &Lj[m], 4);
        uint32_t mb; std::memcpy(&mb, &mag, 4);
        mb |= (sg[m] ^ (sb & 0x80000000u));
        float nm; std::memcpy(&nm, &mb, 4);
        const float old = kFirst ? 0.f : mj[m];
        mj[m] = nm;
        Lj[m] = nm - old;
    }
}
#else
inline void accumulate(const float* Lj, float* min1, float* min2, int* idx1, uint32_t* sg, int j) {
    const int32x4_t jv = vdupq_n_s32(j);
    for (int m = 0; m < Z; m += 4) {
        float32x4_t x = vld1q_f32(Lj + m);
        float32x4_t a = vabsq_f32(x);
        float32x4_t m1 = vld1q_f32(&min1[m]), m2 = vld1q_f32(&min2[m]);
        uint32x4_t lt = vcltq_f32(a, m1);
        vst1q_f32(&min2[m], vbslq_f32(lt, m1, vminq_f32(m2, a)));
        vst1q_s32(&idx1[m], vbslq_s32(lt, jv, vld1q_s32(&idx1[m])));
        vst1q_f32(&min1[m], vminq_f32(m1, a));
        vst1q_u32(&sg[m], veorq_u32(vld1q_u32(&sg[m]), vandq_u32(vreinterpretq_u32_f32(x), vdupq_n_u32(0x80000000u))));
    }
}
template <bool kFirst>
inline void apply(float* Lj, float* mj, const float* min1, const float* min2, const int* idx1, const uint32_t* sg, int j, float alpha) {
    const int32x4_t jv = vdupq_n_s32(j);
    const float32x4_t al = vdupq_n_f32(alpha);
    for (int m = 0; m < Z; m += 4) {
        float32x4_t x = vld1q_f32(Lj + m);
        uint32x4_t isMin = vceqq_s32(vld1q_s32(&idx1[m]), jv);
        float32x4_t mag = vmulq_f32(vbslq_f32(isMin, vld1q_f32(&min2[m]), vld1q_f32(&min1[m])), al);
        uint32x4_t sbit = veorq_u32(vld1q_u32(&sg[m]), vandq_u32(vreinterpretq_u32_f32(x), vdupq_n_u32(0x80000000u)));
        float32x4_t nm = vreinterpretq_f32_u32(vorrq_u32(vreinterpretq_u32_f32(mag), sbit));
        float32x4_t old = kFirst ? vdupq_n_f32(0.f) : vld1q_f32(mj + m);
        vst1q_f32(mj + m, nm);
        vst1q_f32(Lj + m, vsubq_f32(nm, old));   // posterior increment (a variable can appear twice in a layer)
    }
}

// One whole layer, a block of checks at a time, with their min-sum state (min1, min2, idx1, sign) kept in registers across the connections
// instead of being loaded and stored once per connection, and the posterior update done right where the message is made. Every element
// goes through exactly the operations of gather / accumulate / apply / scatter above, in the same order per element; only the order
// between elements changes, and elements share nothing as long as no group appears twice in the layer (the caller checks: such a layer
// takes the pass-per-connection path). So the decisions and iteration counts stay bit for bit those of decodeFast.
struct LayerSrc { float* p; int shift; };   // the posterior values of one connection: check m reads p[(m - shift) mod Z]
#ifndef DECT2_LDPC_V
#define DECT2_LDPC_V 5
#endif
constexpr int kV = DECT2_LDPC_V, kBlk = 4 * kV;   // checks per block (Z = 360 is a multiple); kV independent vectors hide the latency of the min chain
static_assert(Z % kBlk == 0, "block width");
// the kBlk values p[(m0 + l - s) mod Z]: a plain pointer when they do not wrap, else copied into tmp
inline float* blockPtr(const LayerSrc& c, int m0, float* tmp) {
    int a = m0 - c.shift;
    if (a < 0) a += Z;
    if (a + kBlk <= Z) return c.p + a;
    for (int l = 0; l < kBlk; l++) { int i = a + l; if (i >= Z) i -= Z; tmp[l] = c.p[i]; }
    return nullptr;
}
template <bool kFirst>
inline void layerBlocked(const LayerSrc* src, int nc, float* mr, float alpha) {
    const float32x4_t al = vdupq_n_f32(alpha), big = vdupq_n_f32(1e30f);
    const uint32x4_t signMask = vdupq_n_u32(0x80000000u);
    float Lb[64 * kBlk];   // the variable-to-check messages of this block (nc <= 64: the caller checks)
    for (int m0 = 0; m0 < Z; m0 += kBlk) {
        float32x4_t m1[kV], m2[kV];
        int32x4_t ix[kV];
        uint32x4_t sg[kV];
        for (int v = 0; v < kV; v++) { m1[v] = big; m2[v] = big; ix[v] = vdupq_n_s32(-1); sg[v] = vdupq_n_u32(0); }
        for (int j = 0; j < nc; j++) {
            float tmp[kBlk];
            const float* sp = blockPtr(src[j], m0, tmp);
            if (!sp) sp = tmp;
            const float* mj = mr + (size_t)j * Z + m0;
            const int32x4_t jv = vdupq_n_s32(j);
            for (int v = 0; v < kV; v++) {
                float32x4_t x = vld1q_f32(sp + 4 * v);
                if (!kFirst) x = vsubq_f32(x, vld1q_f32(mj + 4 * v));
                vst1q_f32(Lb + j * kBlk + 4 * v, x);
                const float32x4_t a = vabsq_f32(x);
                const uint32x4_t lt = vcltq_f32(a, m1[v]);
                m2[v] = vbslq_f32(lt, m1[v], vminq_f32(m2[v], a)); ix[v] = vbslq_s32(lt, jv, ix[v]); m1[v] = vminq_f32(m1[v], a);
                sg[v] = veorq_u32(sg[v], vandq_u32(vreinterpretq_u32_f32(x), signMask));
            }
        }
        for (int j = 0; j < nc; j++) {
            const int32x4_t jv = vdupq_n_s32(j);
            float* mj = mr + (size_t)j * Z + m0;
            float tmp[kBlk];
            float* dp = blockPtr(src[j], m0, tmp);
            float d[kBlk];
            for (int v = 0; v < kV; v++) {
                const float32x4_t x = vld1q_f32(Lb + j * kBlk + 4 * v);
                const float32x4_t mag = vmulq_f32(vbslq_f32(vceqq_s32(ix[v], jv), m2[v], m1[v]), al);
                const float32x4_t nm = vreinterpretq_f32_u32(vorrq_u32(vreinterpretq_u32_f32(mag), veorq_u32(sg[v], vandq_u32(vreinterpretq_u32_f32(x), signMask))));
                const float32x4_t old = kFirst ? vdupq_n_f32(0.f) : vld1q_f32(mj + 4 * v);
                vst1q_f32(mj + 4 * v, nm);
                const float32x4_t inc = vsubq_f32(nm, old);
                if (dp) vst1q_f32(dp + 4 * v, vaddq_f32(vld1q_f32(dp + 4 * v), inc));
                else vst1q_f32(d + 4 * v, inc);
            }
            if (!dp) {   // the block wraps around the group: element by element (the same single additions)
                int a = m0 - src[j].shift;
                if (a < 0) a += Z;
                for (int l = 0; l < kBlk; l++) { int i = a + l; if (i >= Z) i -= Z; src[j].p[i] += d[l]; }
            }
        }
    }
}
#endif

#if defined(DECT2_X86_DISPATCH)
// The AVX2 twin of layerBlocked: the same per-element operations in the same order (so the same decisions and iteration counts), eight
// checks per vector, kV8 vectors per block (24 checks; 360 = 15 blocks), min-sum state in registers across the connections.
struct LayerSrcX { float* p; int shift; };
constexpr int kV8 = 3, kBlk8 = 8 * kV8;
static_assert(Z % kBlk8 == 0, "block width");
static inline float* blockPtr8(const LayerSrcX& c, int m0, float* tmp) {
    int a = m0 - c.shift;
    if (a < 0) a += Z;
    if (a + kBlk8 <= Z) return c.p + a;
    for (int l = 0; l < kBlk8; l++) { int i = a + l; if (i >= Z) i -= Z; tmp[l] = c.p[i]; }
    return nullptr;
}
template <bool kFirst>
DECT2_AVX2_TARGET static void layerBlockedAvx2(const LayerSrcX* src, int nc, float* mr, float alpha) {
    const __m256 al = _mm256_set1_ps(alpha), big = _mm256_set1_ps(1e30f);
    const __m256 signMask = _mm256_castsi256_ps(_mm256_set1_epi32((int)0x80000000u));
    alignas(32) float Lb[64 * kBlk8];
    for (int m0 = 0; m0 < Z; m0 += kBlk8) {
        __m256 m1[kV8], m2[kV8], sg[kV8];
        __m256i ix[kV8];
        for (int v = 0; v < kV8; v++) { m1[v] = big; m2[v] = big; ix[v] = _mm256_set1_epi32(-1); sg[v] = _mm256_setzero_ps(); }
        for (int j = 0; j < nc; j++) {
            float tmp[kBlk8];
            const float* sp = blockPtr8(src[j], m0, tmp);
            if (!sp) sp = tmp;
            const float* mj = mr + (size_t)j * Z + m0;
            const __m256i jv = _mm256_set1_epi32(j);
            for (int v = 0; v < kV8; v++) {
                __m256 x = _mm256_loadu_ps(sp + 8 * v);
                if (!kFirst) x = _mm256_sub_ps(x, _mm256_loadu_ps(mj + 8 * v));
                _mm256_store_ps(Lb + j * kBlk8 + 8 * v, x);
                const __m256 a = _mm256_andnot_ps(signMask, x);
                const __m256 lt = _mm256_cmp_ps(a, m1[v], _CMP_LT_OQ);
                m2[v] = _mm256_blendv_ps(_mm256_min_ps(m2[v], a), m1[v], lt);
                ix[v] = _mm256_castps_si256(_mm256_blendv_ps(_mm256_castsi256_ps(ix[v]), _mm256_castsi256_ps(jv), lt));
                m1[v] = _mm256_min_ps(m1[v], a);
                sg[v] = _mm256_xor_ps(sg[v], _mm256_and_ps(x, signMask));
            }
        }
        for (int j = 0; j < nc; j++) {
            const __m256i jv = _mm256_set1_epi32(j);
            float* mj = mr + (size_t)j * Z + m0;
            float tmp[kBlk8];
            float* dp = blockPtr8(src[j], m0, tmp);
            alignas(32) float d[kBlk8];
            for (int v = 0; v < kV8; v++) {
                const __m256 x = _mm256_load_ps(Lb + j * kBlk8 + 8 * v);
                const __m256 isMin = _mm256_castsi256_ps(_mm256_cmpeq_epi32(ix[v], jv));
                const __m256 mag = _mm256_mul_ps(_mm256_blendv_ps(m1[v], m2[v], isMin), al);
                const __m256 nm = _mm256_or_ps(mag, _mm256_xor_ps(sg[v], _mm256_and_ps(x, signMask)));
                const __m256 old = kFirst ? _mm256_setzero_ps() : _mm256_loadu_ps(mj + 8 * v);
                _mm256_storeu_ps(mj + 8 * v, nm);
                const __m256 inc = _mm256_sub_ps(nm, old);
                if (dp) _mm256_storeu_ps(dp + 8 * v, _mm256_add_ps(_mm256_loadu_ps(dp + 8 * v), inc));
                else _mm256_store_ps(d + 8 * v, inc);
            }
            if (!dp) {
                int a = m0 - src[j].shift;
                if (a < 0) a += Z;
                for (int l = 0; l < kBlk8; l++) { int i = a + l; if (i >= Z) i -= Z; src[j].p[i] += d[l]; }
            }
        }
    }
}
#endif

struct Work {
    std::vector<float> inf, par, msg, L, min1, min2;
    std::vector<int> idx1;
    std::vector<uint32_t> sg;
    std::vector<size_t> base;
    std::vector<uint8_t> infS, parS, blockable;
};
Work& work() { static thread_local Work w; return w; }

template <bool kAvx>
DECT2_SIMD_BODY bool decodeBody(const LdpcCode& code, const std::vector<float>& llr, int maxIter, std::vector<uint8_t>& hard, int* iters, float alpha) {
    const int q = code.q(), G = code.groups(), k = code.k(), n = code.n();
    const auto& layers = code.layers();
    Work& w = work();
    // posterior LLRs: information groups [G][Z]; parity groups [q][Z+1] with a leading pad slot (index -1)
    w.inf.resize((size_t)G * Z); w.par.resize((size_t)q * (Z + 1));
    float* inf = w.inf.data();
    auto P = [&](int r) { return w.par.data() + (size_t)r * (Z + 1) + 1; };
    std::copy(llr.begin(), llr.begin() + (size_t)G * Z, inf);
    for (int r = 0; r < q; r++) {
        float* p = P(r);
        p[-1] = kBig;
        for (int m = 0; m < Z; m++) p[m] = llr[(size_t)(k + r + q * m)];
    }
    // check-to-variable messages per layer and connection (+2 parity connections); not cleared: the first iteration writes them all
    w.base.assign((size_t)q + 1, 0);
    for (int r = 0; r < q; r++) w.base[(size_t)r + 1] = w.base[(size_t)r] + (layers[(size_t)r].size() + 2) * Z;
    w.msg.resize(w.base[(size_t)q]);
    w.min1.resize(Z); w.min2.resize(Z); w.idx1.resize(Z); w.sg.resize(Z);
    w.infS.resize((size_t)G * Z); w.parS.resize((size_t)q * Z);
    float* min1 = w.min1.data(); float* min2 = w.min2.data(); int* idx1 = w.idx1.data(); uint32_t* sg = w.sg.data();
#if (defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)) || defined(DECT2_X86_DISPATCH)
    // the layers whose connections all reach different groups take the blocked path (layerBlocked)
    w.blockable.assign((size_t)q, 0);
    for (int r = 0; r < q && q > 1 && (kAvx || DECT2_NOT_X86); r++) {
        const auto& cn = layers[(size_t)r];
        bool ok = cn.size() + 2 <= 64;
        for (size_t a = 0; a < cn.size() && ok; a++) for (size_t b = a + 1; b < cn.size() && ok; b++) ok = cn[a].group != cn[b].group;
        w.blockable[(size_t)r] = ok;
    }
#endif
    int it = 0;
    bool ok = false;
    for (; it < maxIter; it++) {
        const bool first = it == 0;
        for (int r = 0; r < q; r++) {
            const auto& cn = layers[(size_t)r];
            const int nc = (int)cn.size() + 2;
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
            if (w.blockable[(size_t)r]) {
                LayerSrc src[64];
                for (size_t j = 0; j < cn.size(); j++) src[j] = {inf + (size_t)cn[j].group * Z, cn[j].shift};
                src[cn.size()] = {P(r), 0};
                src[cn.size() + 1] = {r > 0 ? P(r - 1) : P(q - 1) - 1, 0};
                float* mr = w.msg.data() + w.base[(size_t)r];
                if (first) layerBlocked<true>(src, nc, mr, alpha);
                else layerBlocked<false>(src, nc, mr, alpha);
                P(q - 1)[-1] = kBig;
                continue;
            }
#endif
#if defined(DECT2_X86_DISPATCH)
            if (kAvx && w.blockable[(size_t)r]) {
                LayerSrcX src[64];
                for (size_t j = 0; j < cn.size(); j++) src[j] = {inf + (size_t)cn[j].group * Z, cn[j].shift};
                src[cn.size()] = {P(r), 0};
                src[cn.size() + 1] = {r > 0 ? P(r - 1) : P(q - 1) - 1, 0};
                float* mr = w.msg.data() + w.base[(size_t)r];
                if (first) layerBlockedAvx2<true>(src, nc, mr, alpha);
                else layerBlockedAvx2<false>(src, nc, mr, alpha);
                P(q - 1)[-1] = kBig;
                continue;
            }
#endif
            if (w.L.size() < (size_t)nc * Z) w.L.resize((size_t)nc * Z);
            float* Lb = w.L.data();
            float* mr = w.msg.data() + w.base[(size_t)r];
            // gather variable-to-check messages (every element of L is written)
            for (int j = 0; j < nc; j++) {
                float* Lj = Lb + (size_t)j * Z;
                const float* mj = mr + (size_t)j * Z;
                if (j < (int)cn.size()) {
                    const float* src = inf + (size_t)cn[(size_t)j].group * Z;
                    const int s = cn[(size_t)j].shift;
                    // check m reads variable n = (m - s) mod Z
                    if (first) { std::copy(src, src + (Z - s), Lj + s); std::copy(src + (Z - s), src + Z, Lj); }
                    else {
                        for (int m = s; m < Z; m++) Lj[m] = src[m - s] - mj[m];
                        for (int m = 0; m < s; m++) Lj[m] = src[m - s + Z] - mj[m];
                    }
                } else {
                    const float* src = (j == (int)cn.size()) ? P(r) : (r > 0 ? P(r - 1) : P(q - 1) - 1);
                    if (first) std::copy(src, src + Z, Lj);
                    else for (int m = 0; m < Z; m++) Lj[m] = src[m] - mj[m];
                }
            }
            // min-sum over the nc connections, for all 360 checks of the layer at once
            for (int m = 0; m < Z; m++) { min1[m] = 1e30f; min2[m] = 1e30f; idx1[m] = -1; sg[m] = 0; }
            for (int j = 0; j < nc; j++) accumulate(Lb + (size_t)j * Z, min1, min2, idx1, sg, j);
            for (int j = 0; j < nc; j++) {
                float* Lj = Lb + (size_t)j * Z;
                float* mj = mr + (size_t)j * Z;
                if (first) apply<true>(Lj, mj, min1, min2, idx1, sg, j, alpha);
                else apply<false>(Lj, mj, min1, min2, idx1, sg, j, alpha);
                if (j < (int)cn.size()) {
                    float* dst = inf + (size_t)cn[(size_t)j].group * Z;
                    const int s = cn[(size_t)j].shift;
                    for (int m = s; m < Z; m++) dst[m - s] += Lj[m];
                    for (int m = 0; m < s; m++) dst[m - s + Z] += Lj[m];
                } else {
                    float* dst = (j == (int)cn.size()) ? P(r) : (r > 0 ? P(r - 1) : P(q - 1) - 1);
                    for (int m = 0; m < Z; m++) dst[m] += Lj[m];
                }
            }
            P(q - 1)[-1] = kBig;   // the non-existent parity bit before the first one stays known
        }
        // parity checks on the hard decisions: check (r, m) is parity bits (r, m) and (r - 1, m) (for r = 0: (q - 1, m - 1)) and the information
        // bits of the layer's connections; all 360 checks of a layer together on sign bytes, stopping at the first layer with a failed check
        uint8_t* is = w.infS.data();
        uint8_t* ps = w.parS.data();
        for (int i = 0; i < G * Z; i++) is[i] = inf[i] < 0;
        for (int r = 0; r < q; r++) { const float* p = P(r); uint8_t* o = ps + (size_t)r * Z; for (int m = 0; m < Z; m++) o[m] = p[m] < 0; }
        ok = true;
        uint8_t acc[Z];
        for (int r = 0; r < q && ok; r++) {
            const uint8_t* pr = ps + (size_t)r * Z;
            if (r > 0) { const uint8_t* pp = pr - Z; for (int m = 0; m < Z; m++) acc[m] = pr[m] ^ pp[m]; }
            else { const uint8_t* pp = ps + (size_t)(q - 1) * Z; acc[0] = pr[0]; for (int m = 1; m < Z; m++) acc[m] = pr[m] ^ pp[m - 1]; }
            for (const auto& c : layers[(size_t)r]) {
                const uint8_t* src = is + (size_t)c.group * Z;
                const int s = c.shift;
                for (int m = s; m < Z; m++) acc[m] ^= src[m - s];
                for (int m = 0; m < s; m++) acc[m] ^= src[m - s + Z];
            }
            uint8_t any = 0;
            for (int m = 0; m < Z; m++) any |= acc[m];
            ok = any == 0;
        }
        if (ok) { it++; break; }
    }
    hard.assign((size_t)n, 0);
    for (int i = 0; i < G * Z; i++) hard[(size_t)i] = inf[i] < 0;
    for (int r = 0; r < q; r++) { const float* p = P(r); for (int m = 0; m < Z; m++) hard[(size_t)(k + r + q * m)] = p[m] < 0; }
    if (iters) *iters = it;
    return ok;
}
#if defined(DECT2_X86_DISPATCH)
DECT2_AVX2_TARGET bool decodeAvx2(const LdpcCode& code, const std::vector<float>& llr, int maxIter, std::vector<uint8_t>& hard, int* iters, float alpha) {
    return decodeBody<true>(code, llr, maxIter, hard, iters, alpha);
}
#endif
} // namespace

bool t2LdpcDecode(const LdpcCode& code, const std::vector<float>& llr, int maxIter, std::vector<uint8_t>& hard, int* iters, float alpha) {
    // a code without the quasi-cyclic 360 structure (none in DVB-T2): the general decoder
    if (code.q() <= 0 || code.groups() * Z != code.k() || (int)llr.size() != code.n()) return code.decodeFast(llr, maxIter, hard, iters, alpha);
#if defined(DECT2_X86_DISPATCH)
    if (cpuHasAvx2()) return decodeAvx2(code, llr, maxIter, hard, iters, alpha);
#endif
    return decodeBody<false>(code, llr, maxIter, hard, iters, alpha);
}

} // namespace dect2
