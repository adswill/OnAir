#include "dect2/ldpc.h"
#include "dect2/simd.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
#include <arm_neon.h>
#include <cstdint>
#include <vector>
#endif

namespace dect2 {

#if !(defined(__ARM_NEON) && !defined(DECT2_NO_SIMD))
namespace {
// Branch-free min-sum kernels over one connection (Z checks at a time); the compiler vectorises them (AVX2 clone on x86-64).
DECT2_MULTIVERSION void minSumAccumulate(const float* __restrict Lj, float* __restrict min1, float* __restrict min2,
                                         int* __restrict idx1, uint32_t* __restrict sg, int j, int Z) {
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
DECT2_MULTIVERSION void minSumApply(float* __restrict Lj, float* __restrict mj, const float* __restrict min1, const float* __restrict min2,
                                    const int* __restrict idx1, const uint32_t* __restrict sg, int j, int Z, float alpha) {
    for (int m = 0; m < Z; m++) {
        const float mag = (idx1[m] == j ? min2[m] : min1[m]) * alpha;
        uint32_t sb; std::memcpy(&sb, &Lj[m], 4);
        uint32_t mb; std::memcpy(&mb, &mag, 4);
        mb |= (sg[m] ^ (sb & 0x80000000u));
        float nm; std::memcpy(&nm, &mb, 4);
        const float old = mj[m];
        mj[m] = nm;
        Lj[m] = nm - old;
    }
}
} // namespace
#endif

LdpcCode::LdpcCode(int k, int n, const std::vector<std::vector<int>>& rows) : k_(k), n_(n), m_(n - k) {
    const int q = m_ / 360;
    q_ = q;
    groups_ = (int)rows.size();
    layers_.assign(q, {});
    for (int g = 0; g < groups_; g++) for (int a : rows[g]) layers_[a % q].push_back({g, a / q});
    std::vector<std::vector<int>> chk(m_);
    int im = 0;
    for (auto& row : rows) {
        for (int c = 0; c < 360; c++, im++) {
            for (int a : row) {
                int p = (a + c * q) % m_;
                encP_.push_back(p);
                encD_.push_back(im);
                chk[p].push_back(im);
            }
        }
    }
    // zig-zag parity structure: check j involves parity bits j and j-1
    for (int j = 0; j < m_; j++) {
        chk[j].push_back(k_ + j);
        if (j > 0) chk[j].push_back(k_ + j - 1);
    }
    chkStart_.resize(m_ + 1);
    chkStart_[0] = 0;
    for (int j = 0; j < m_; j++) chkStart_[j + 1] = chkStart_[j] + (int)chk[j].size();
    chkVar_.reserve(chkStart_[m_]);
    for (int j = 0; j < m_; j++) chkVar_.insert(chkVar_.end(), chk[j].begin(), chk[j].end());
}

void LdpcCode::encode(std::vector<uint8_t>& bits) const {
    bits.resize(n_);
    std::fill(bits.begin() + k_, bits.end(), 0);
    for (size_t e = 0; e < encP_.size(); e++) bits[k_ + encP_[e]] ^= bits[encD_[e]];
    for (int j = 1; j < m_; j++) bits[k_ + j] ^= bits[k_ + j - 1];
}

bool LdpcCode::decode(const std::vector<float>& llr, int maxIter, std::vector<uint8_t>& hard, int* iters) const {
    const int E = chkStart_[m_];
    std::vector<float> c2v(E, 0.f), v2c(E, 0.f), total(n_);
    hard.assign(n_, 0);
    const float alpha = 0.8f; // normalisation factor
    int it = 0;
    bool ok = false;
    for (; it < maxIter; it++) {
        // variable -> check: total LLR minus the check's own contribution
        std::copy(llr.begin(), llr.end(), total.begin());
        for (int e = 0; e < E; e++) total[chkVar_[e]] += c2v[e];
        for (int e = 0; e < E; e++) v2c[e] = total[chkVar_[e]] - c2v[e];
        // check update (min-sum)
        for (int j = 0; j < m_; j++) {
            int s = chkStart_[j], t = chkStart_[j + 1];
            float min1 = 1e30f, min2 = 1e30f;
            int idx1 = -1;
            int sign = 0;
            for (int e = s; e < t; e++) {
                float a = std::fabs(v2c[e]);
                if (v2c[e] < 0) sign ^= 1;
                if (a < min1) { min2 = min1; min1 = a; idx1 = e; }
                else if (a < min2) min2 = a;
            }
            for (int e = s; e < t; e++) {
                float mag = (e == idx1 ? min2 : min1) * alpha;
                int sg = sign ^ (v2c[e] < 0 ? 1 : 0);
                c2v[e] = sg ? -mag : mag;
            }
        }
        // decisions and syndrome
        std::copy(llr.begin(), llr.end(), total.begin());
        for (int e = 0; e < E; e++) total[chkVar_[e]] += c2v[e];
        for (int i = 0; i < n_; i++) hard[i] = total[i] < 0;
        ok = true;
        for (int j = 0; j < m_ && ok; j++) {
            int p = 0;
            for (int e = chkStart_[j]; e < chkStart_[j + 1]; e++) p ^= hard[chkVar_[e]];
            if (p) ok = false;
        }
        if (ok) { it++; break; }
    }
    if (iters) *iters = it;
    return ok;
}

DECT2_MULTIVERSION bool LdpcCode::decodeFast(const std::vector<float>& llr, int maxIter, std::vector<uint8_t>& hard, int* iters, float alpha) const {
    constexpr int Z = 360;
    constexpr float kBig = 1e4f;
    const int q = q_, G = groups_;
    // posterior LLRs: information groups [G][Z]; parity groups [q][Z+1] with a leading pad slot (index -1)
    std::vector<float> inf((size_t)G * Z), par((size_t)q * (Z + 1));
    auto P = [&](int r) { return par.data() + (size_t)r * (Z + 1) + 1; };
    for (int i = 0; i < G * Z; i++) inf[i] = llr[i];
    for (int r = 0; r < q; r++) {
        float* p = P(r);
        p[-1] = kBig;
        for (int m = 0; m < Z; m++) p[m] = llr[k_ + r + q * m];
    }
    // check-to-variable messages per layer and connection (+2 parity connections)
    std::vector<size_t> base(q + 1, 0);
    for (int r = 0; r < q; r++) base[r + 1] = base[r] + (layers_[r].size() + 2) * Z;
    std::vector<float> msg(base[q], 0.f);
    std::vector<float> L, min1(Z), min2(Z);
    std::vector<int> idx1(Z);
    std::vector<uint32_t> sg(Z);
    int it = 0;
    bool ok = false;
    for (; it < maxIter; it++) {
        for (int r = 0; r < q; r++) {
            const auto& cn = layers_[r];
            const int nc = (int)cn.size() + 2;
            L.assign((size_t)nc * Z, 0.f);
            float* mr = msg.data() + base[r];
            // gather variable-to-check messages
            for (int j = 0; j < nc; j++) {
                float* Lj = L.data() + (size_t)j * Z;
                const float* mj = mr + (size_t)j * Z;
                if (j < (int)cn.size()) {
                    const float* src = inf.data() + (size_t)cn[j].group * Z;
                    const int s = cn[j].shift;
                    // check m reads variable n = (m - s) mod Z
                    for (int m = s; m < Z; m++) Lj[m] = src[m - s] - mj[m];
                    for (int m = 0; m < s; m++) Lj[m] = src[m - s + Z] - mj[m];
                } else {
                    const float* src = (j == (int)cn.size()) ? P(r) : (r > 0 ? P(r - 1) : P(q - 1) - 1);
                    for (int m = 0; m < Z; m++) Lj[m] = src[m] - mj[m];
                }
            }
            // min-sum over the nc connections, for all 360 checks of the layer at once
            for (int m = 0; m < Z; m++) { min1[m] = 1e30f; min2[m] = 1e30f; idx1[m] = -1; sg[m] = 0; }
            for (int j = 0; j < nc; j++) {
                const float* Lj = L.data() + (size_t)j * Z;
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
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
#else
                minSumAccumulate(Lj, min1.data(), min2.data(), idx1.data(), sg.data(), j, Z);
#endif
            }
            for (int j = 0; j < nc; j++) {
                float* Lj = L.data() + (size_t)j * Z;
                float* mj = mr + (size_t)j * Z;
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
                const int32x4_t jv = vdupq_n_s32(j);
                const float32x4_t al = vdupq_n_f32(alpha);
                for (int m = 0; m < Z; m += 4) {
                    float32x4_t x = vld1q_f32(Lj + m);
                    uint32x4_t isMin = vceqq_s32(vld1q_s32(&idx1[m]), jv);
                    float32x4_t mag = vmulq_f32(vbslq_f32(isMin, vld1q_f32(&min2[m]), vld1q_f32(&min1[m])), al);
                    uint32x4_t sbit = veorq_u32(vld1q_u32(&sg[m]), vandq_u32(vreinterpretq_u32_f32(x), vdupq_n_u32(0x80000000u)));
                    float32x4_t nm = vreinterpretq_f32_u32(vorrq_u32(vreinterpretq_u32_f32(mag), sbit));
                    float32x4_t old = vld1q_f32(mj + m);
                    vst1q_f32(mj + m, nm);
                    vst1q_f32(Lj + m, vsubq_f32(nm, old)); // posterior increment (a variable can appear twice in a layer)
                }
#else
                minSumApply(Lj, mj, min1.data(), min2.data(), idx1.data(), sg.data(), j, Z, alpha);
#endif
                if (j < (int)cn.size()) {
                    float* dst = inf.data() + (size_t)cn[j].group * Z;
                    const int s = cn[j].shift;
                    for (int m = s; m < Z; m++) dst[m - s] += Lj[m];
                    for (int m = 0; m < s; m++) dst[m - s + Z] += Lj[m];
                } else {
                    float* dst = (j == (int)cn.size()) ? P(r) : (r > 0 ? P(r - 1) : P(q - 1) - 1);
                    for (int m = 0; m < Z; m++) dst[m] += Lj[m];
                }
            }
            P(q - 1)[-1] = kBig; // the non-existent parity bit before the first one stays known
        }
        // syndrome check on hard decisions
        ok = true;
        for (int r = 0; r < q && ok; r++) {
            const auto& cn = layers_[r];
            for (int m = 0; m < Z; m++) {
                int p = (P(r)[m] < 0) ^ (r > 0 ? (P(r - 1)[m] < 0) : (m > 0 ? (P(q - 1)[m - 1] < 0) : 0));
                for (const auto& c : cn) {
                    int n = m - c.shift; if (n < 0) n += Z;
                    p ^= inf[(size_t)c.group * Z + n] < 0;
                }
                if (p) { ok = false; break; }
            }
        }
        if (ok) { it++; break; }
    }
    hard.assign(n_, 0);
    for (int i = 0; i < G * Z; i++) hard[i] = inf[i] < 0;
    for (int r = 0; r < q; r++) for (int m = 0; m < Z; m++) hard[k_ + r + q * m] = P(r)[m] < 0;
    if (iters) *iters = it;
    return ok;
}

} // namespace dect2
