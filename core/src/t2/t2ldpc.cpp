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

namespace dect2 {

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
#endif

struct Work {
    std::vector<float> inf, par, msg, L, min1, min2;
    std::vector<int> idx1;
    std::vector<uint32_t> sg;
    std::vector<size_t> base;
    std::vector<uint8_t> infS, parS;
};
Work& work() { static thread_local Work w; return w; }

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
    int it = 0;
    bool ok = false;
    for (; it < maxIter; it++) {
        const bool first = it == 0;
        for (int r = 0; r < q; r++) {
            const auto& cn = layers[(size_t)r];
            const int nc = (int)cn.size() + 2;
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
    return decodeBody(code, llr, maxIter, hard, iters, alpha);
}
#endif
} // namespace

bool t2LdpcDecode(const LdpcCode& code, const std::vector<float>& llr, int maxIter, std::vector<uint8_t>& hard, int* iters, float alpha) {
    // a code without the quasi-cyclic 360 structure (none in DVB-T2): the general decoder
    if (code.q() <= 0 || code.groups() * Z != code.k() || (int)llr.size() != code.n()) return code.decodeFast(llr, maxIter, hard, iters, alpha);
#if defined(DECT2_X86_DISPATCH)
    if (cpuHasAvx2()) return decodeAvx2(code, llr, maxIter, hard, iters, alpha);
#endif
    return decodeBody(code, llr, maxIter, hard, iters, alpha);
}

} // namespace dect2
