// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The K = 7 Viterbi decoder of DVB-T / ISDB-T (see fec_proto.h): NEON / SSE2 / AVX2 / portable. The Reed-Solomon, BCH and LDPC parts of the shared forward error
// correction are Rust (fec_proto_rs.cpp over rust/onair_fec). The Rust NEON Viterbi kernel (ONAIR_RUST_VITERBI, aarch64) is ~4% slower than this one, so it stays opt-in
// (cmake -DDECT2_RUST_VITERBI=ON) and this C++ decoder is the one in use by default.
#include "dect2/fec/fec_proto.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#if !defined(ONAIR_RUST_VITERBI) || !defined(__aarch64__)
#if defined(__SSE2__) && !defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
#include <emmintrin.h>
#if defined(__GNUC__) && defined(__x86_64__)
#include <immintrin.h>
#define DECT2_VIT_AVX2 1
#endif
#endif
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
#include <arm_neon.h>
#endif
namespace dect2::fecp {
namespace {
struct Trellis {
    // for each new state ns and predecessor bit b: expected code bits (x,y) as +1 / -1 signs
    int8_t sx[64][2], sy[64][2];
    Trellis() {
        for (int ns = 0; ns < 64; ns++) {
            const int u = ns >> 5;
            for (int b = 0; b < 2; b++) {
                const int s = ((ns & 31) << 1) | b;           // predecessor state
                const unsigned reg = ((unsigned)u << 6) | (unsigned)s;
                sx[ns][b] = __builtin_parity(reg & 0x79) ? -1 : 1;
                sy[ns][b] = __builtin_parity(reg & 0x5B) ? -1 : 1;
            }
        }
    }
};
const Trellis& trellis() { static Trellis t; return t; }
// the decision bits of a run (8 bytes a step, about 130 kB for a DVB-T chunk): kept with the thread, not allocated and zeroed for every run
// (every element is written before it is read)
inline uint64_t* decisionScratch(size_t steps) {
    static thread_local std::vector<uint64_t> buf;
    if (buf.size() < steps) buf.resize(steps);
    return buf.data();
}

#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
#include <arm_neon.h>
#include <array>
#include <cstdint>
#include <vector>
// NEON path. States 2j and 2j+1 feed new states j and j+32; both branch outputs flip when either the new bit or the oldest
// state bit flips (both generator polynomials have those taps), so one metric M_j = +/-sx +/- sy per butterfly is enough:
//   new[j]    = max(old[2j] + M, old[2j+1] - M)      new[j+32] = max(old[2j] - M, old[2j+1] + M)
struct NeonTables {
    int16_t sigX[32], sigY[32];
    NeonTables() {
        for (int j = 0; j < 32; j++) {
            const unsigned reg = (unsigned)(2 * j);
            sigX[j] = __builtin_parity(reg & 0x79) ? -1 : 1;
            sigY[j] = __builtin_parity(reg & 0x5B) ? -1 : 1;
        }
    }
};
const NeonTables& neonTables() { static NeonTables t; return t; }


// One trellis step: N (states 8q..8q+7 in N[q]) -> the new metrics in N, the 64 decision bits to *dec.
// The butterfly metrics of the four groups are M0, -M0, M2, -M2 (the sign tables of groups 1 and 3 are those of 0 and 2 negated), so only
// two are computed and the negation is folded into the add / subtract. The decision masks are narrowed with one uzp1 per pair (the low byte
// of each 16-bit lane) instead of two xtn, and stored straight from the vector register.
static inline __attribute__((always_inline)) void vitStep(int16x8_t (&N)[8], const int16x8_t sx0, const int16x8_t sy0, const int16x8_t sx2,
                                                          const int16x8_t sy2, const uint8x16_t wt, int16_t a, int16_t b, uint64_t* dec) {
    const int16x8_t M0 = vmlaq_n_s16(vmulq_n_s16(sx0, a), sy0, b);
    const int16x8_t M2 = vmlaq_n_s16(vmulq_n_s16(sx2, a), sy2, b);
    int16x8_t nn[8];
    uint16x8_t lo[4], hi[4];
    for (int q = 0; q < 4; q++) {
        const int16x8x2_t o = vuzpq_s16(N[2 * q], N[2 * q + 1]);   // even states, odd states
        const int16x8_t M = (q & 2) ? M2 : M0;
        const int16x8_t A = o.val[0], B = o.val[1];
        // groups 1 and 3: the metric is -M, so A + (-M) = A - M etc.
        const int16x8_t p0 = (q & 1) ? vqsubq_s16(A, M) : vqaddq_s16(A, M), p1 = (q & 1) ? vqaddq_s16(B, M) : vqsubq_s16(B, M);
        const int16x8_t r0 = (q & 1) ? vqaddq_s16(A, M) : vqsubq_s16(A, M), r1 = (q & 1) ? vqsubq_s16(B, M) : vqaddq_s16(B, M);
        nn[q] = vmaxq_s16(p0, p1);
        nn[4 + q] = vmaxq_s16(r0, r1);
        lo[q] = vcgtq_s16(p1, p0);
        hi[q] = vcgtq_s16(r1, r0);
    }
    // the 64 decision bits (lane i of the 64 comparison results -> bit i): narrow to bytes, weight by 1,2,4..128 and add
    // neighbours three times, which leaves each group of eight lanes as one byte
    const uint8x16_t c0 = vandq_u8(vuzp1q_u8(vreinterpretq_u8_u16(lo[0]), vreinterpretq_u8_u16(lo[1])), wt);
    const uint8x16_t c1 = vandq_u8(vuzp1q_u8(vreinterpretq_u8_u16(lo[2]), vreinterpretq_u8_u16(lo[3])), wt);
    const uint8x16_t c2 = vandq_u8(vuzp1q_u8(vreinterpretq_u8_u16(hi[0]), vreinterpretq_u8_u16(hi[1])), wt);
    const uint8x16_t c3 = vandq_u8(vuzp1q_u8(vreinterpretq_u8_u16(hi[2]), vreinterpretq_u8_u16(hi[3])), wt);
    uint8x16_t r = vpaddq_u8(vpaddq_u8(c0, c1), vpaddq_u8(c2, c3));
    r = vpaddq_u8(r, r);
    vst1q_lane_u64(dec, vreinterpretq_u64_u8(r), 0);
    for (int q = 0; q < 8; q++) N[q] = nn[q];
}

void viterbiRangeCpp(const int8_t* soft, size_t steps, std::vector<uint8_t>& bits, long* metricOut) {
    const NeonTables& T = neonTables();
    uint64_t* dec = decisionScratch(steps);
    long off = 0; // total subtracted by the renormalisation: the absolute path metric is off + pm
    alignas(16) int16_t pm[64] = {};
    const int16x8_t sx0 = vld1q_s16(T.sigX), sy0 = vld1q_s16(T.sigY), sx2 = vld1q_s16(T.sigX + 16), sy2 = vld1q_s16(T.sigY + 16);
    // The 64 path metrics live in eight registers for the whole run (no store and reload per step: that round trip made the
    // chain of steps latency-bound). N[q] holds states 8q..8q+7.
    int16x8_t N[8];
    for (int q = 0; q < 8; q++) N[q] = vdupq_n_s16(0);
    static const uint8_t w8[16] = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
    const uint8x16_t wt = vld1q_u8(w8);
    uint64_t* dp = dec;
    size_t t = 0;
    // Sixteen steps per pass, then renormalise. int16 never saturates: the metrics of any two states differ by at most 6 steps' worth of
    // branch-metric range (6 x 2 x 254 = 3048, any state reaches any other in K - 1 = 6 steps) and each step moves a metric by at most
    // 254, so 16 steps after a renormalisation every metric is within +-(3048 + 16 x 254) = +-7112. Without saturation every operation is
    // exact integer arithmetic, so where the common offset is taken out changes nothing: the same decisions, and off + pm the same metric.
    for (; t + 16 <= steps; t += 16) {
#pragma clang loop unroll(full)
        for (int g = 0; g < 16; g += 4) {
            const int8_t* s = soft + 2 * (t + g);
            vitStep(N, sx0, sy0, sx2, sy2, wt, s[0], s[1], dp + t + g);
            vitStep(N, sx0, sy0, sx2, sy2, wt, s[2], s[3], dp + t + g + 1);
            vitStep(N, sx0, sy0, sx2, sy2, wt, s[4], s[5], dp + t + g + 2);
            vitStep(N, sx0, sy0, sx2, sy2, wt, s[6], s[7], dp + t + g + 3);
        }
        int16x8_t mx = vmaxq_s16(vmaxq_s16(vmaxq_s16(N[0], N[1]), vmaxq_s16(N[2], N[3])), vmaxq_s16(vmaxq_s16(N[4], N[5]), vmaxq_s16(N[6], N[7])));
        const int16_t top = vmaxvq_s16(mx);
        off += top;
        const int16x8_t m = vdupq_n_s16(top);
        for (int q = 0; q < 8; q++) N[q] = vsubq_s16(N[q], m);
    }
    for (; t < steps; t++) vitStep(N, sx0, sy0, sx2, sy2, wt, soft[2 * t], soft[2 * t + 1], dp + t);
    for (int q = 0; q < 8; q++) vst1q_s16(pm + 8 * q, N[q]);
    int state = 0;
    int bm = pm[0];
    for (int s = 1; s < 64; s++) if (pm[s] > bm) { bm = pm[s]; state = s; }
    bits.assign(steps, 0);
    for (size_t t = steps; t-- > 0;) {
        bits[t] = (uint8_t)(state >> 5);
        const int b = (int)((dec[t] >> state) & 1);
        state = ((state & 31) << 1) | b;
    }
    if (metricOut) *metricOut = off + bm;
}
#elif defined(__SSE2__) && !defined(DECT2_NO_SIMD)
// SSE2 path (every x86-64 CPU): the same butterflies and int16 metrics as the NEON path above, eight states per register.
struct SseTables {
    alignas(16) int16_t sigX[32], sigY[32];
    SseTables() {
        for (int j = 0; j < 32; j++) {
            const unsigned reg = (unsigned)(2 * j);
            sigX[j] = __builtin_parity(reg & 0x79) ? -1 : 1;
            sigY[j] = __builtin_parity(reg & 0x5B) ? -1 : 1;
        }
    }
};
const SseTables& sseTables() { static SseTables t; return t; }

// states 16q..16q+15: the even ones (old[2j]) to A, the odd ones (old[2j+1]) to B, like NEON's vld2q_s16
inline void deinterleave(const __m128i x0, const __m128i x1, __m128i& A, __m128i& B) {
    A = _mm_packs_epi32(_mm_srai_epi32(_mm_slli_epi32(x0, 16), 16), _mm_srai_epi32(_mm_slli_epi32(x1, 16), 16));
    B = _mm_packs_epi32(_mm_srai_epi32(x0, 16), _mm_srai_epi32(x1, 16));
}
// one bit per lane of a comparison result, lane i to bit i
inline uint8_t maskBits(__m128i m) { return (uint8_t)_mm_movemask_epi8(_mm_packs_epi16(m, _mm_setzero_si128())); }
inline int16_t maxLane(__m128i v) {
    v = _mm_max_epi16(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(1, 0, 3, 2)));
    v = _mm_max_epi16(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(2, 3, 0, 1)));
    v = _mm_max_epi16(v, _mm_shufflelo_epi16(v, _MM_SHUFFLE(2, 3, 0, 1)));
    return (int16_t)_mm_cvtsi128_si32(v);
}

void viterbiRangeSse(const int8_t* soft, size_t steps, std::vector<uint8_t>& bits, long* metricOut) {
    const SseTables& T = sseTables();
    uint64_t* dec = decisionScratch(steps);
    long off = 0; // total subtracted by the renormalisation: the absolute path metric is off + pm
    alignas(16) int16_t pm[64] = {};
    __m128i sigX[4], sigY[4];
    for (int q = 0; q < 4; q++) { sigX[q] = _mm_load_si128((const __m128i*)(T.sigX + 8 * q)); sigY[q] = _mm_load_si128((const __m128i*)(T.sigY + 8 * q)); }
    // the 64 path metrics stay in eight registers (N[q] = states 8q..8q+7) instead of a store and reload per step
    __m128i N[8];
    for (int q = 0; q < 8; q++) N[q] = _mm_setzero_si128();
    for (size_t t = 0; t < steps; t++) {
        const __m128i a = _mm_set1_epi16(soft[2 * t]), b = _mm_set1_epi16(soft[2 * t + 1]);
        __m128i nn[8];
        uint64_t d = 0;
        for (int q = 0; q < 4; q++) {
            __m128i A, B;
            deinterleave(N[2 * q], N[2 * q + 1], A, B);
            const __m128i M = _mm_add_epi16(_mm_mullo_epi16(sigX[q], a), _mm_mullo_epi16(sigY[q], b));
            const __m128i p0 = _mm_adds_epi16(A, M), p1 = _mm_subs_epi16(B, M);
            const __m128i r0 = _mm_subs_epi16(A, M), r1 = _mm_adds_epi16(B, M);
            nn[q] = _mm_max_epi16(p0, p1);
            nn[4 + q] = _mm_max_epi16(r0, r1);
            d |= (uint64_t)maskBits(_mm_cmpgt_epi16(p1, p0)) << (8 * q);
            d |= (uint64_t)maskBits(_mm_cmpgt_epi16(r1, r0)) << (32 + 8 * q);
        }
        dec[t] = d;
        // renormalise every 4 steps so int16 never saturates
        if ((t & 3) == 3) {
            __m128i mx = nn[0];
            for (int q = 1; q < 8; q++) mx = _mm_max_epi16(mx, nn[q]);
            const int16_t top = maxLane(mx);
            off += top;
            const __m128i m = _mm_set1_epi16(top);
            for (int q = 0; q < 8; q++) N[q] = _mm_sub_epi16(nn[q], m);
        } else for (int q = 0; q < 8; q++) N[q] = nn[q];
    }
    for (int q = 0; q < 8; q++) _mm_store_si128((__m128i*)(pm + 8 * q), N[q]);
    int state = 0;
    int bm = pm[0];
    for (int s = 1; s < 64; s++) if (pm[s] > bm) { bm = pm[s]; state = s; }
    bits.assign(steps, 0);
    for (size_t t = steps; t-- > 0;) {
        bits[t] = (uint8_t)(state >> 5);
        const int b = (int)((dec[t] >> state) & 1);
        state = ((state & 31) << 1) | b;
    }
    if (metricOut) *metricOut = off + bm;
}
#ifdef DECT2_VIT_AVX2
// The same trellis with 256-bit registers (16 states each): identical decisions and metrics, half the instructions.
__attribute__((target("avx2")))
inline void deinterleave256(const __m256i x0, const __m256i x1, __m256i& A, __m256i& B) {
    // packs works within the 128-bit halves, so the quarters come out as 0 2 1 3 and are put back in order
    const __m256i a = _mm256_packs_epi32(_mm256_srai_epi32(_mm256_slli_epi32(x0, 16), 16), _mm256_srai_epi32(_mm256_slli_epi32(x1, 16), 16));
    const __m256i b = _mm256_packs_epi32(_mm256_srai_epi32(x0, 16), _mm256_srai_epi32(x1, 16));
    A = _mm256_permute4x64_epi64(a, 0xD8);
    B = _mm256_permute4x64_epi64(b, 0xD8);
}
// the comparison results of two registers (32 lanes) as 32 bits, lane i to bit i
__attribute__((target("avx2")))
inline uint32_t maskBits256(__m256i m0, __m256i m1) {
    return (uint32_t)_mm256_movemask_epi8(_mm256_permute4x64_epi64(_mm256_packs_epi16(m0, m1), 0xD8));
}

__attribute__((target("avx2")))
void viterbiRangeAvx2(const int8_t* soft, size_t steps, std::vector<uint8_t>& bits, long* metricOut) {
    const SseTables& T = sseTables();
    uint64_t* dec = decisionScratch(steps);
    long off = 0;
    alignas(32) int16_t pm[64] = {};
    const __m256i sigX0 = _mm256_loadu_si256((const __m256i*)T.sigX), sigX1 = _mm256_loadu_si256((const __m256i*)(T.sigX + 16));
    const __m256i sigY0 = _mm256_loadu_si256((const __m256i*)T.sigY), sigY1 = _mm256_loadu_si256((const __m256i*)(T.sigY + 16));
    __m256i N0 = _mm256_setzero_si256(), N1 = N0, N2 = N0, N3 = N0;   // states 0-15, 16-31, 32-47, 48-63
    for (size_t t = 0; t < steps; t++) {
        const __m256i a = _mm256_set1_epi16(soft[2 * t]), b = _mm256_set1_epi16(soft[2 * t + 1]);
        __m256i A0, B0, A1, B1;
        deinterleave256(N0, N1, A0, B0);   // old states 0-31: new states 0-15 and 32-47
        deinterleave256(N2, N3, A1, B1);   // old states 32-63: new states 16-31 and 48-63
        const __m256i M0 = _mm256_add_epi16(_mm256_mullo_epi16(sigX0, a), _mm256_mullo_epi16(sigY0, b));
        const __m256i M1 = _mm256_add_epi16(_mm256_mullo_epi16(sigX1, a), _mm256_mullo_epi16(sigY1, b));
        const __m256i p00 = _mm256_adds_epi16(A0, M0), p10 = _mm256_subs_epi16(B0, M0);
        const __m256i r00 = _mm256_subs_epi16(A0, M0), r10 = _mm256_adds_epi16(B0, M0);
        const __m256i p01 = _mm256_adds_epi16(A1, M1), p11 = _mm256_subs_epi16(B1, M1);
        const __m256i r01 = _mm256_subs_epi16(A1, M1), r11 = _mm256_adds_epi16(B1, M1);
        __m256i n0 = _mm256_max_epi16(p00, p10), n1 = _mm256_max_epi16(p01, p11);
        __m256i n2 = _mm256_max_epi16(r00, r10), n3 = _mm256_max_epi16(r01, r11);
        dec[t] = (uint64_t)maskBits256(_mm256_cmpgt_epi16(p10, p00), _mm256_cmpgt_epi16(p11, p01))
               | (uint64_t)maskBits256(_mm256_cmpgt_epi16(r10, r00), _mm256_cmpgt_epi16(r11, r01)) << 32;
        if ((t & 3) == 3) {
            __m256i mx = _mm256_max_epi16(_mm256_max_epi16(n0, n1), _mm256_max_epi16(n2, n3));
            const int16_t top = maxLane(_mm_max_epi16(_mm256_castsi256_si128(mx), _mm256_extracti128_si256(mx, 1)));
            off += top;
            const __m256i m = _mm256_set1_epi16(top);
            n0 = _mm256_sub_epi16(n0, m); n1 = _mm256_sub_epi16(n1, m); n2 = _mm256_sub_epi16(n2, m); n3 = _mm256_sub_epi16(n3, m);
        }
        N0 = n0; N1 = n1; N2 = n2; N3 = n3;
    }
    _mm256_store_si256((__m256i*)pm, N0); _mm256_store_si256((__m256i*)(pm + 16), N1);
    _mm256_store_si256((__m256i*)(pm + 32), N2); _mm256_store_si256((__m256i*)(pm + 48), N3);
    int state = 0;
    int bm = pm[0];
    for (int s = 1; s < 64; s++) if (pm[s] > bm) { bm = pm[s]; state = s; }
    bits.assign(steps, 0);
    for (size_t t = steps; t-- > 0;) {
        bits[t] = (uint8_t)(state >> 5);
        const int b = (int)((dec[t] >> state) & 1);
        state = ((state & 31) << 1) | b;
    }
    if (metricOut) *metricOut = off + bm;
}
#endif

void viterbiRangeCpp(const int8_t* soft, size_t steps, std::vector<uint8_t>& bits, long* metricOut) {
#ifdef DECT2_VIT_AVX2
    static const bool avx2 = __builtin_cpu_supports("avx2");
    if (avx2) return viterbiRangeAvx2(soft, steps, bits, metricOut);
#endif
    viterbiRangeSse(soft, steps, bits, metricOut);
}
#else
void viterbiRangeCpp(const int8_t* soft, size_t steps, std::vector<uint8_t>& bits, long* metricOut) {
    const Trellis& T = trellis();
    uint64_t* dec = decisionScratch(steps);
    long off = 0;
    int32_t pm[64] = {}, nm[64];
    for (size_t t = 0; t < steps; t++) {
        const int sx = soft[2 * t], sy = soft[2 * t + 1];
        uint64_t d = 0;
        for (int ns = 0; ns < 64; ns++) {
            const int p0 = ((ns & 31) << 1), p1 = p0 | 1;
            const int m0 = pm[p0] + T.sx[ns][0] * sx + T.sy[ns][0] * sy;
            const int m1 = pm[p1] + T.sx[ns][1] * sx + T.sy[ns][1] * sy;
            if (m1 > m0) { nm[ns] = m1; d |= 1ull << ns; } else nm[ns] = m0;
        }
        int best = nm[0];
        for (int s = 1; s < 64; s++) best = std::max(best, nm[s]);
        off += best;
        for (int s = 0; s < 64; s++) pm[s] = nm[s] - best;
        dec[t] = d;
    }
    int state = 0, bm = pm[0];
    for (int s = 1; s < 64; s++) if (pm[s] > bm) { bm = pm[s]; state = s; }
    bits.assign(steps, 0);
    for (size_t t = steps; t-- > 0;) {
        bits[t] = (uint8_t)(state >> 5);
        const int b = (int)((dec[t] >> state) & 1);
        state = ((state & 31) << 1) | b;
    }
    if (metricOut) *metricOut = off + bm;
}
#endif
} // namespace


void viterbiRange(const int8_t* soft, size_t steps, std::vector<uint8_t>& bits, long* metricOut) { viterbiRangeCpp(soft, steps, bits, metricOut); }
} // namespace dect2::fecp
#endif // C++ Viterbi

// ------------------------------------------------------------------------------------------------------------------------------------------
// 8-bit Viterbi, two runs side by side (fec_proto.h). States 16q..16q+15 in N[q]; new states j and j+32 come from old 2j and 2j+1 (vuzp
// of two registers), with the butterfly metric M_j = sx_j a + sy_j b (|M| <= 62 for soft values within +-31). Saturating adds: a metric
// that falls 128 below the best one sticks at the floor, which only happens to paths that cannot survive. Renormalised (best = 0) after
// every second step: the best metric grows by at most 62 a step, so it never reaches the top of the range.
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
#include <arm_neon.h>
namespace dect2::fecp {
namespace {
struct V8Tab {
    int8_t sx[32], sy[32];
    V8Tab() { for (int j = 0; j < 32; j++) { const unsigned r = (unsigned)(2 * j); sx[j] = __builtin_parity(r & 0x79) ? -1 : 1; sy[j] = __builtin_parity(r & 0x5B) ? -1 : 1; } }
};
const V8Tab& v8tab() { static V8Tab t; return t; }
struct V8Run {
    int8x16_t N[4];
    uint64_t* dec = nullptr;
    long off = 0;
};
inline __attribute__((always_inline)) void v8step(V8Run& r, const int8x16_t (&sx)[2], const int8x16_t (&sy)[2], uint8x16_t wt, int8_t a, int8_t b, uint64_t* dec) {
    const int8x16_t va = vdupq_n_s8(a), vb = vdupq_n_s8(b);
    int8x16_t nn[4];
    uint8x16_t lo[2], hi[2];
    for (int g = 0; g < 2; g++) {
        const int8x16_t M = vmlaq_s8(vmulq_s8(sx[g], va), sy[g], vb);
        const int8x16_t A = vuzp1q_s8(r.N[2 * g], r.N[2 * g + 1]), B = vuzp2q_s8(r.N[2 * g], r.N[2 * g + 1]);
        const int8x16_t p0 = vqaddq_s8(A, M), p1 = vqsubq_s8(B, M), r0 = vqsubq_s8(A, M), r1 = vqaddq_s8(B, M);
        nn[g] = vmaxq_s8(p0, p1); nn[2 + g] = vmaxq_s8(r0, r1);
        lo[g] = vcgtq_s8(p1, p0); hi[g] = vcgtq_s8(r1, r0);
    }
    // decision bit of state s -> bit s of the step's word (weights 1..128 per group of eight lanes, three pairwise sums)
    uint8x16_t d = vpaddq_u8(vpaddq_u8(vandq_u8(lo[0], wt), vandq_u8(lo[1], wt)), vpaddq_u8(vandq_u8(hi[0], wt), vandq_u8(hi[1], wt)));
    d = vpaddq_u8(d, d);
    vst1q_lane_u64(dec, vreinterpretq_u64_u8(d), 0);
    for (int q = 0; q < 4; q++) r.N[q] = nn[q];
}
inline void v8renorm(V8Run& r) {
    const int8_t top = vmaxvq_s8(vmaxq_s8(vmaxq_s8(r.N[0], r.N[1]), vmaxq_s8(r.N[2], r.N[3])));
    r.off += top;
    const int8x16_t m = vdupq_n_s8(top);
    for (int q = 0; q < 4; q++) r.N[q] = vqsubq_s8(r.N[q], m);
}
void v8finish(V8Run& r, size_t steps, std::vector<uint8_t>& bits, long* metric) {
    int8_t pm[64];
    for (int q = 0; q < 4; q++) vst1q_s8(pm + 16 * q, r.N[q]);
    int state = 0, bm = pm[0];
    for (int s = 1; s < 64; s++) if (pm[s] > bm) { bm = pm[s]; state = s; }
    bits.assign(steps, 0);
    for (size_t t = steps; t-- > 0;) {
        bits[t] = (uint8_t)(state >> 5);
        state = ((state & 31) << 1) | (int)((r.dec[t] >> state) & 1);
    }
    if (metric) *metric = r.off + bm;
}
}   // namespace

bool viterbi8x2Available() { return true; }

void viterbiRange8x2(const int8_t* a, size_t na, const int8_t* b, size_t nb, std::vector<uint8_t>& bitsA, std::vector<uint8_t>& bitsB, long* metricA, long* metricB) {
    if (!b) nb = 0;
    const V8Tab& T = v8tab();
    const int8x16_t sx[2] = {vld1q_s8(T.sx), vld1q_s8(T.sx + 16)}, sy[2] = {vld1q_s8(T.sy), vld1q_s8(T.sy + 16)};
    static const uint8_t w8[16] = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
    const uint8x16_t wt = vld1q_u8(w8);
    static thread_local std::vector<uint64_t> dA, dB;
    if (dA.size() < na) dA.resize(na);
    if (dB.size() < nb) dB.resize(nb);
    V8Run A, B;
    for (int q = 0; q < 4; q++) { A.N[q] = vdupq_n_s8(0); B.N[q] = A.N[q]; }
    A.dec = dA.data(); B.dec = dB.data();
    const size_t both = std::min(na, nb) & ~(size_t)1;
    size_t t = 0;
    for (; t < both; t += 2) {   // two steps of each run, interleaved, then both renormalised
        v8step(A, sx, sy, wt, a[2 * t], a[2 * t + 1], A.dec + t);
        v8step(B, sx, sy, wt, b[2 * t], b[2 * t + 1], B.dec + t);
        v8step(A, sx, sy, wt, a[2 * t + 2], a[2 * t + 3], A.dec + t + 1);
        v8step(B, sx, sy, wt, b[2 * t + 2], b[2 * t + 3], B.dec + t + 1);
        v8renorm(A); v8renorm(B);
    }
    auto rest = [&](V8Run& r, const int8_t* s, size_t n, size_t from) {   // the longer run on its own (renormalised after every second step)
        for (size_t k = from; k < n; k++) {
            v8step(r, sx, sy, wt, s[2 * k], s[2 * k + 1], r.dec + k);
            if ((k - from) & 1) v8renorm(r);
        }
    };
    rest(A, a, na, t);
    v8finish(A, na, bitsA, metricA);
    if (nb) { rest(B, b, nb, t); v8finish(B, nb, bitsB, metricB); }
}
}   // namespace dect2::fecp
// ------------------------------------------------------------------------------------------------------------------------------------------
// The same 8-bit decoder on x86-64 with AVX2 (32 states per register, runtime dispatch): states 0..31 in N0, 32..63 in N1; new states j and j+32
// come from old 2j and 2j+1 (byte shuffle + 64-bit unpack + qword permute). Same arithmetic, tie rule and decision words as the NEON code above.
#elif defined(DECT2_VIT_AVX2)
namespace dect2::fecp {
namespace {
#define V8A __attribute__((target("avx2")))
struct V8Tab {
    int8_t sx[32], sy[32];
    V8Tab() { for (int j = 0; j < 32; j++) { const unsigned r = (unsigned)(2 * j); sx[j] = __builtin_parity(r & 0x79) ? -1 : 1; sy[j] = __builtin_parity(r & 0x5B) ? -1 : 1; } }
};
const V8Tab& v8tab() { static V8Tab t; return t; }
struct V8Run {
    __m256i N0, N1;
    uint64_t* dec = nullptr;
    long off = 0;
};
V8A inline __attribute__((always_inline)) void v8step(V8Run& r, __m256i sx, __m256i sy, __m256i evod, int8_t a, int8_t b, uint64_t* dec) {
    const __m256i M = _mm256_add_epi8(_mm256_sign_epi8(_mm256_set1_epi8(a), sx), _mm256_sign_epi8(_mm256_set1_epi8(b), sy));
    const __m256i s0 = _mm256_shuffle_epi8(r.N0, evod), s1 = _mm256_shuffle_epi8(r.N1, evod);
    const __m256i A = _mm256_permute4x64_epi64(_mm256_unpacklo_epi64(s0, s1), 0xD8), B = _mm256_permute4x64_epi64(_mm256_unpackhi_epi64(s0, s1), 0xD8);
    const __m256i p0 = _mm256_adds_epi8(A, M), p1 = _mm256_subs_epi8(B, M), r0 = _mm256_subs_epi8(A, M), r1 = _mm256_adds_epi8(B, M);
    r.N0 = _mm256_max_epi8(p0, p1); r.N1 = _mm256_max_epi8(r0, r1);
    const uint32_t lo = (uint32_t)_mm256_movemask_epi8(_mm256_cmpgt_epi8(p1, p0)), hi = (uint32_t)_mm256_movemask_epi8(_mm256_cmpgt_epi8(r1, r0));
    *dec = (uint64_t)lo | ((uint64_t)hi << 32);
}
V8A inline __attribute__((always_inline)) void v8renorm(V8Run& r) {
    __m256i x = _mm256_max_epi8(r.N0, r.N1);
    __m128i y = _mm_max_epi8(_mm256_castsi256_si128(x), _mm256_extracti128_si256(x, 1));
    y = _mm_max_epi8(y, _mm_srli_si128(y, 8)); y = _mm_max_epi8(y, _mm_srli_si128(y, 4)); y = _mm_max_epi8(y, _mm_srli_si128(y, 2)); y = _mm_max_epi8(y, _mm_srli_si128(y, 1));
    const int8_t top = (int8_t)_mm_cvtsi128_si32(y);
    r.off += top;
    const __m256i m = _mm256_set1_epi8(top);
    r.N0 = _mm256_subs_epi8(r.N0, m); r.N1 = _mm256_subs_epi8(r.N1, m);
}
V8A void v8finish(V8Run& r, size_t steps, std::vector<uint8_t>& bits, long* metric) {
    alignas(32) int8_t pm[64];
    _mm256_store_si256((__m256i*)pm, r.N0); _mm256_store_si256((__m256i*)(pm + 32), r.N1);
    int state = 0, bm = pm[0];
    for (int s = 1; s < 64; s++) if (pm[s] > bm) { bm = pm[s]; state = s; }
    bits.assign(steps, 0);
    for (size_t t = steps; t-- > 0;) {
        bits[t] = (uint8_t)(state >> 5);
        state = ((state & 31) << 1) | (int)((r.dec[t] >> state) & 1);
    }
    if (metric) *metric = r.off + bm;
}
V8A void rest(V8Run& r, const int8_t* s, size_t n, size_t from, __m256i sx, __m256i sy, __m256i evod) {
    for (size_t k = from; k < n; k++) {
        v8step(r, sx, sy, evod, s[2 * k], s[2 * k + 1], r.dec + k);
        if ((k - from) & 1) v8renorm(r);
    }
}
V8A void run8x2Avx2(const int8_t* a, size_t na, const int8_t* b, size_t nb, std::vector<uint8_t>& bitsA, std::vector<uint8_t>& bitsB, long* metricA, long* metricB) {
    if (!b) nb = 0;
    const V8Tab& T = v8tab();
    const __m256i sx = _mm256_loadu_si256((const __m256i*)T.sx), sy = _mm256_loadu_si256((const __m256i*)T.sy);
    const __m256i evod = _mm256_setr_epi8(0, 2, 4, 6, 8, 10, 12, 14, 1, 3, 5, 7, 9, 11, 13, 15, 0, 2, 4, 6, 8, 10, 12, 14, 1, 3, 5, 7, 9, 11, 13, 15);
    static thread_local std::vector<uint64_t> dA, dB;
    if (dA.size() < na) dA.resize(na);
    if (dB.size() < nb) dB.resize(nb);
    V8Run A, B;
    A.N0 = A.N1 = B.N0 = B.N1 = _mm256_setzero_si256();
    A.dec = dA.data(); B.dec = dB.data();
    const size_t both = std::min(na, nb) & ~(size_t)1;
    size_t t = 0;
    for (; t < both; t += 2) {
        v8step(A, sx, sy, evod, a[2 * t], a[2 * t + 1], A.dec + t);
        v8step(B, sx, sy, evod, b[2 * t], b[2 * t + 1], B.dec + t);
        v8step(A, sx, sy, evod, a[2 * t + 2], a[2 * t + 3], A.dec + t + 1);
        v8step(B, sx, sy, evod, b[2 * t + 2], b[2 * t + 3], B.dec + t + 1);
        v8renorm(A); v8renorm(B);
    }
    rest(A, a, na, t, sx, sy, evod);
    v8finish(A, na, bitsA, metricA);
    if (nb) { rest(B, b, nb, t, sx, sy, evod); v8finish(B, nb, bitsB, metricB); }
}
bool haveAvx2() { static const bool v = __builtin_cpu_supports("avx2"); return v; }
}   // namespace
#if defined(_WIN32)
// MinGW-w64 keeps the x64 stack 16-byte aligned, but GCC spills ymm registers to the stack with vmovdqa as if it were 32-byte aligned (GCC bug 54412;
// force_align_arg_pointer and -mstackrealign do not help on Windows). On a worker thread that faulted: access violation at 0xffffffffffffffff, the DVB-T
// crash of #324. So the AVX2 decoder is entered through this trampoline, which puts rsp on a 32-byte boundary first (the call then enters with rsp = 24 mod 32,
// the assumption GCC makes). The .seh directives keep the exception unwinding (std::bad_alloc from the vectors) working through it.
extern "C" void dect2_vit_call32(void (*fn)(void*), void* ctx);
__asm__(".text\n.globl dect2_vit_call32\n.def dect2_vit_call32; .scl 2; .type 32; .endef\n.seh_proc dect2_vit_call32\ndect2_vit_call32:\n"
        "pushq %rbp\n.seh_pushreg %rbp\nmovq %rsp, %rbp\n.seh_setframe %rbp, 0\n.seh_endprologue\n"
        "subq $64, %rsp\nandq $-32, %rsp\nmovq %rcx, %rax\nmovq %rdx, %rcx\ncall *%rax\n"
        "movq %rbp, %rsp\npopq %rbp\nret\n.seh_endproc\n");
struct V8Call { const int8_t *a, *b; size_t na, nb; std::vector<uint8_t> *bitsA, *bitsB; long *metricA, *metricB; };
static void v8trampoline(void* p) { V8Call& c = *(V8Call*)p; run8x2Avx2(c.a, c.na, c.b, c.nb, *c.bitsA, *c.bitsB, c.metricA, c.metricB); }
#endif
bool viterbi8x2Available() { return haveAvx2(); }
void viterbiRange8x2(const int8_t* a, size_t na, const int8_t* b, size_t nb, std::vector<uint8_t>& bitsA, std::vector<uint8_t>& bitsB, long* metricA, long* metricB) {
#if defined(_WIN32)
    if (haveAvx2()) { V8Call c{a, b, na, nb, &bitsA, &bitsB, metricA, metricB}; dect2_vit_call32(v8trampoline, &c); return; }
#else
    if (haveAvx2()) { run8x2Avx2(a, na, b, nb, bitsA, bitsB, metricA, metricB); return; }
#endif
    viterbiRange(a, na, bitsA, metricA);
    if (b) viterbiRange(b, nb, bitsB, metricB);
}
}   // namespace dect2::fecp
#else
namespace dect2::fecp {
bool viterbi8x2Available() { return false; }
void viterbiRange8x2(const int8_t* a, size_t na, const int8_t* b, size_t nb, std::vector<uint8_t>& bitsA, std::vector<uint8_t>& bitsB, long* metricA, long* metricB) {
    viterbiRange(a, na, bitsA, metricA);   // (no NEON: callers check viterbi8x2Available() and scale for viterbiRange instead)
    if (b) viterbiRange(b, nb, bitsB, metricB);
}
}   // namespace dect2::fecp
#endif
