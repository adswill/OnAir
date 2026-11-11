#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
#include <arm_neon.h>
#elif defined(__SSE2__) && !defined(DECT2_NO_SIMD)
#include <emmintrin.h>
#endif
#include "dect2/dsp/simd.h"
#if defined(DECT2_X86_DISPATCH)
#include <immintrin.h>
#endif
#include "dect2/atsc/atsc.h"
#include "dect2/fec/fec_proto.h"
#include "dect2/common/stage_prof.h"
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

// ---------------------------------------------------------------- Reed-Solomon (207,187): core/src/fec/fec_proto.cpp (C++) or the Rust crate onair_fec
void rsEncode(const uint8_t* in, uint8_t* out) { fecp::rs207Encode(in, out); }
int rsDecode(uint8_t* r) { return fecp::rs207Decode(r); }

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
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
static_assert(kLanes == 12, "three NEON vectors of four lanes");
// The same arithmetic as the portable loop below (bit-exact), written with intrinsics: the state metrics stay in registers, the decisions are narrowed
// to bytes in the register instead of through scalar selects.
void viterbiLanes(const float* xt, int n, uint8_t* dec, int* fin) {
    float32x4_t pm[8][3];
    for (int s = 0; s < 8; s++) for (int v = 0; v < 3; v++) pm[s][v] = vdupq_n_f32(0);
    const uint32x4_t two = vdupq_n_u32(2);
    for (int t = 0; t < n; t++) {
        const float* xv = xt + (size_t)t * kLanes;
        const float32x4_t x0 = vld1q_f32(xv), x1 = vld1q_f32(xv + 4), x2 = vld1q_f32(xv + 8);
        float32x4_t d[8][3];
        for (int s = 0; s < 8; s++) {
            const float32x4_t lv = vdupq_n_f32((float)(2 * s - 7));
            const float32x4_t a0 = vsubq_f32(x0, lv), a1 = vsubq_f32(x1, lv), a2 = vsubq_f32(x2, lv);
            d[s][0] = vmulq_f32(a0, a0); d[s][1] = vmulq_f32(a1, a1); d[s][2] = vmulq_f32(a2, a2);
        }
        uint8_t* dp = dec + (size_t)t * 8 * kLanes;
        float32x4_t nm[8][3], mn[3] = {vdupq_n_f32(1e30f), vdupq_n_f32(1e30f), vdupq_n_f32(1e30f)};
        for (int ns = 0; ns < 8; ns++) {
            uint16x4_t dv[3];
            for (int v = 0; v < 3; v++) {
                const float32x4_t m0 = vaddq_f32(pm[kVit.from[ns][0]][v], d[kVit.sym[ns][0]][v]);
                const float32x4_t m1 = vaddq_f32(pm[kVit.from[ns][1]][v], d[kVit.sym[ns][1]][v]);
                const float32x4_t m2 = vaddq_f32(pm[kVit.from[ns][2]][v], d[kVit.sym[ns][2]][v]);
                const float32x4_t m3 = vaddq_f32(pm[kVit.from[ns][3]][v], d[kVit.sym[ns][3]][v]);
                const uint32x4_t b01 = vcltq_f32(m1, m0), b23 = vcltq_f32(m3, m2);
                const float32x4_t a = vminq_f32(m0, m1), c = vminq_f32(m2, m3);
                const uint32x4_t cw = vcltq_f32(c, a);
                nm[ns][v] = vminq_f32(a, c);
                mn[v] = vminq_f32(mn[v], nm[ns][v]);
                dv[v] = vmovn_u32(vbslq_u32(cw, vaddq_u32(two, vshrq_n_u32(b23, 31)), vshrq_n_u32(b01, 31)));
            }
            vst1_u8(dp + ns * kLanes, vmovn_u16(vcombine_u16(dv[0], dv[1])));
            vst1_lane_u32((uint32_t*)(dp + ns * kLanes + 8), vreinterpret_u32_u8(vmovn_u16(vcombine_u16(dv[2], dv[2]))), 0);
        }
        for (int s = 0; s < 8; s++) for (int v = 0; v < 3; v++) pm[s][v] = vsubq_f32(nm[s][v], mn[v]);
    }
    for (int l = 0; l < kLanes; l++) {
        int state = 0;
        for (int s = 1; s < 8; s++) if (pm[s][l >> 2][l & 3] < pm[state][l >> 2][l & 3]) state = s;
        fin[l] = state;
    }
}
#elif defined(__SSE2__) && !defined(DECT2_NO_SIMD)
static_assert(kLanes == 12, "three SSE vectors of four lanes");
// The same arithmetic as the portable loop below (bit-exact: the same adds, compares and minimums), with SSE2 intrinsics: the state metrics stay in registers.
static void viterbiLanesSse2(const float* xt, int n, uint8_t* dec, int* fin) {
    __m128 pm[8][3];
    for (int s = 0; s < 8; s++) for (int v = 0; v < 3; v++) pm[s][v] = _mm_setzero_ps();
    const __m128i two = _mm_set1_epi32(2);
    for (int t = 0; t < n; t++) {
        const float* xv = xt + (size_t)t * kLanes;
        const __m128 x0 = _mm_loadu_ps(xv), x1 = _mm_loadu_ps(xv + 4), x2 = _mm_loadu_ps(xv + 8);
        __m128 d[8][3];
        for (int s = 0; s < 8; s++) {
            const __m128 lv = _mm_set1_ps((float)(2 * s - 7));
            const __m128 a0 = _mm_sub_ps(x0, lv), a1 = _mm_sub_ps(x1, lv), a2 = _mm_sub_ps(x2, lv);
            d[s][0] = _mm_mul_ps(a0, a0); d[s][1] = _mm_mul_ps(a1, a1); d[s][2] = _mm_mul_ps(a2, a2);
        }
        uint8_t* dp = dec + (size_t)t * 8 * kLanes;
        __m128 nm[8][3], mn[3] = {_mm_set1_ps(1e30f), _mm_set1_ps(1e30f), _mm_set1_ps(1e30f)};
        for (int ns = 0; ns < 8; ns++) {
            __m128i dv[3];
            for (int v = 0; v < 3; v++) {
                const __m128 m0 = _mm_add_ps(pm[kVit.from[ns][0]][v], d[kVit.sym[ns][0]][v]);
                const __m128 m1 = _mm_add_ps(pm[kVit.from[ns][1]][v], d[kVit.sym[ns][1]][v]);
                const __m128 m2 = _mm_add_ps(pm[kVit.from[ns][2]][v], d[kVit.sym[ns][2]][v]);
                const __m128 m3 = _mm_add_ps(pm[kVit.from[ns][3]][v], d[kVit.sym[ns][3]][v]);
                const __m128i b01 = _mm_castps_si128(_mm_cmplt_ps(m1, m0)), b23 = _mm_castps_si128(_mm_cmplt_ps(m3, m2));
                const __m128 a = _mm_min_ps(m0, m1), c = _mm_min_ps(m2, m3);
                const __m128i cw = _mm_castps_si128(_mm_cmplt_ps(c, a));
                nm[ns][v] = _mm_min_ps(a, c);
                mn[v] = _mm_min_ps(mn[v], nm[ns][v]);
                const __m128i hi = _mm_add_epi32(two, _mm_srli_epi32(b23, 31)), lo = _mm_srli_epi32(b01, 31);
                dv[v] = _mm_or_si128(_mm_and_si128(cw, hi), _mm_andnot_si128(cw, lo));
            }
            const __m128i p01 = _mm_packs_epi32(dv[0], dv[1]), p2 = _mm_packs_epi32(dv[2], dv[2]);
            const __m128i b = _mm_packus_epi16(p01, p2);   // bytes 0..7 = lanes 0..7, bytes 8..11 = lanes 8..11
            _mm_storel_epi64((__m128i*)(dp + ns * kLanes), b);
            const int32_t w = _mm_cvtsi128_si32(_mm_srli_si128(b, 8));
            memcpy(dp + ns * kLanes + 8, &w, 4);
        }
        for (int s = 0; s < 8; s++) for (int v = 0; v < 3; v++) pm[s][v] = _mm_sub_ps(nm[s][v], mn[v]);
    }
    alignas(16) float f[8][kLanes];
    for (int s = 0; s < 8; s++) for (int v = 0; v < 3; v++) _mm_store_ps(&f[s][4 * v], pm[s][v]);
    for (int l = 0; l < kLanes; l++) {
        int state = 0;
        for (int s = 1; s < 8; s++) if (f[s][l] < f[state][l]) state = s;
        fin[l] = state;
    }
}
#if defined(DECT2_X86_DISPATCH)
// AVX2: the same operations as the SSE2 version (bit-exact), lanes 0-7 in one 256-bit vector and lanes 8-11 in a 128-bit one: two vectors per
// operation instead of three
DECT2_AVX2_TARGET static void viterbiLanesAvx2(const float* xt, int n, uint8_t* dec, int* fin) {
    __m256 pa[8]; __m128 pb[8];
    for (int s = 0; s < 8; s++) { pa[s] = _mm256_setzero_ps(); pb[s] = _mm_setzero_ps(); }
    const __m256i twoA = _mm256_set1_epi32(2);
    const __m128i twoB = _mm_set1_epi32(2);
    for (int t = 0; t < n; t++) {
        const float* xv = xt + (size_t)t * kLanes;
        const __m256 xa = _mm256_loadu_ps(xv);
        const __m128 xb = _mm_loadu_ps(xv + 8);
        __m256 da[8]; __m128 db[8];
        for (int s = 0; s < 8; s++) {
            const float lv = (float)(2 * s - 7);
            const __m256 a = _mm256_sub_ps(xa, _mm256_set1_ps(lv));
            const __m128 b = _mm_sub_ps(xb, _mm_set1_ps(lv));
            da[s] = _mm256_mul_ps(a, a); db[s] = _mm_mul_ps(b, b);
        }
        uint8_t* dp = dec + (size_t)t * 8 * kLanes;
        __m256 nma[8], mna = _mm256_set1_ps(1e30f);
        __m128 nmb[8], mnb = _mm_set1_ps(1e30f);
        for (int ns = 0; ns < 8; ns++) {
            const int f0 = kVit.from[ns][0], f1 = kVit.from[ns][1], f2 = kVit.from[ns][2], f3 = kVit.from[ns][3];
            const int s0 = kVit.sym[ns][0], s1 = kVit.sym[ns][1], s2 = kVit.sym[ns][2], s3 = kVit.sym[ns][3];
            __m256i dva;
            {
                const __m256 m0 = _mm256_add_ps(pa[f0], da[s0]), m1 = _mm256_add_ps(pa[f1], da[s1]);
                const __m256 m2 = _mm256_add_ps(pa[f2], da[s2]), m3 = _mm256_add_ps(pa[f3], da[s3]);
                const __m256i b01 = _mm256_castps_si256(_mm256_cmp_ps(m1, m0, _CMP_LT_OS)), b23 = _mm256_castps_si256(_mm256_cmp_ps(m3, m2, _CMP_LT_OS));
                const __m256 a = _mm256_min_ps(m0, m1), c = _mm256_min_ps(m2, m3);
                const __m256i cw = _mm256_castps_si256(_mm256_cmp_ps(c, a, _CMP_LT_OS));
                nma[ns] = _mm256_min_ps(a, c);
                mna = _mm256_min_ps(mna, nma[ns]);
                const __m256i hi = _mm256_add_epi32(twoA, _mm256_srli_epi32(b23, 31)), lo = _mm256_srli_epi32(b01, 31);
                dva = _mm256_or_si256(_mm256_and_si256(cw, hi), _mm256_andnot_si256(cw, lo));
            }
            __m128i dvb;
            {
                const __m128 m0 = _mm_add_ps(pb[f0], db[s0]), m1 = _mm_add_ps(pb[f1], db[s1]);
                const __m128 m2 = _mm_add_ps(pb[f2], db[s2]), m3 = _mm_add_ps(pb[f3], db[s3]);
                const __m128i b01 = _mm_castps_si128(_mm_cmplt_ps(m1, m0)), b23 = _mm_castps_si128(_mm_cmplt_ps(m3, m2));
                const __m128 a = _mm_min_ps(m0, m1), c = _mm_min_ps(m2, m3);
                const __m128i cw = _mm_castps_si128(_mm_cmplt_ps(c, a));
                nmb[ns] = _mm_min_ps(a, c);
                mnb = _mm_min_ps(mnb, nmb[ns]);
                const __m128i hi = _mm_add_epi32(twoB, _mm_srli_epi32(b23, 31)), lo = _mm_srli_epi32(b01, 31);
                dvb = _mm_or_si128(_mm_and_si128(cw, hi), _mm_andnot_si128(cw, lo));
            }
            const __m128i p01 = _mm_packs_epi32(_mm256_castsi256_si128(dva), _mm256_extracti128_si256(dva, 1)), p2 = _mm_packs_epi32(dvb, dvb);
            const __m128i b = _mm_packus_epi16(p01, p2);   // bytes 0..7 = lanes 0..7, bytes 8..11 = lanes 8..11
            _mm_storel_epi64((__m128i*)(dp + ns * kLanes), b);
            const int32_t w = _mm_cvtsi128_si32(_mm_srli_si128(b, 8));
            memcpy(dp + ns * kLanes + 8, &w, 4);
        }
        for (int s = 0; s < 8; s++) { pa[s] = _mm256_sub_ps(nma[s], mna); pb[s] = _mm_sub_ps(nmb[s], mnb); }
    }
    alignas(32) float f[8][kLanes];
    for (int s = 0; s < 8; s++) { _mm256_storeu_ps(&f[s][0], pa[s]); _mm_storeu_ps(&f[s][8], pb[s]); }
    for (int l = 0; l < kLanes; l++) {
        int state = 0;
        for (int s = 1; s < 8; s++) if (f[s][l] < f[state][l]) state = s;
        fin[l] = state;
    }
}
#endif
void viterbiLanes(const float* xt, int n, uint8_t* dec, int* fin) {
#if defined(DECT2_X86_DISPATCH)
    if (cpuHasAvx2()) { viterbiLanesAvx2(xt, n, dec, fin); return; }
#endif
    viterbiLanesSse2(xt, n, dec, fin);
}
#else
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
#endif
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
    DECT2_STAGE_VAR(stVit, "atsc.trellis");
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
    stVit.stop();
    DECT2_STAGE_VAR(stDil, "atsc.deinterleave");
    dil_.syncCommutator();
    std::vector<uint8_t> de(bytes.size());
    dil_.process(bytes.data(), de.data(), bytes.size());
    stDil.stop();
    DECT2_STAGE("atsc.rs");
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
