// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/stage_prof.h"
#include "dect2/atsc3/atsc3_bicm.h"
#include "atsc3_data_tables.h"
#include "atsc3_ldpc.h"
#include "dect2/dsp/ldpc.h"
#include "dect2/dsp/gpu_ldpc.h"
#include "dect2/dsp/gpu_policy.h"
#include <cstdlib>
#include "dect2/atsc3/atsc3_l1.h"
#include "dect2/dsp/simd.h"
#include "dect2/t2/t2fec.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include "dect2/common/bits.h"
#include "dect2/common/mathconst.h"

namespace dect2 {
namespace atsc3 {

namespace {

constexpr float kLlrLimit = 20.f;

// Table 6.8 and 6.9: block interleaver type (0 = A, 1 = B), rates 2/15 .. 13/15
const char* kTypes64[6] = {"AAAAAAAAAAAA", "AAABAABBAAAA", "AAAAABABBAAB", "AAABBBBABBAB", "AAABABABBBAA", "AAAAABAAAAAA"};
const char* kTypes16[4] = {"AAAABBABAAAA", "AAAABBABABAB", "AAAABBABAAAA", "AAAABAAAABAA"};

int modIndex(int eta) { return eta / 2 - 1; }   // 2, 4, 6, 8, 10, 12 -> 0..5

int grayInverse(int g) { return (int)dect2::bits::grayInverse((uint32_t)g); }

std::vector<cf32> buildPoints(int eta, int rate15) {
    if (eta == 2) {
        const float s = dect2::math::k1OverSqrt2f;
        return {cf32(s, s), cf32(-s, s), cf32(s, -s), cf32(-s, -s)};
    }
    const float* v = nucVectors(eta, rate15);
    if (!v) return {};
    const int M = 1 << eta;
    std::vector<cf32> x(M);
    if (eta <= 8) {
        int b = M / 4;
        for (int i = 0; i < b; i++) {
            cf32 w(v[2 * i], v[2 * i + 1]);
            x[i] = w; x[b + i] = -std::conj(w); x[2 * b + i] = std::conj(w); x[3 * b + i] = -w;
        }
        return x;
    }
    // 1D non-uniform: the real part uses the odd label bits, the imaginary part the even ones; sign bit first, then a Gray code of the magnitude
    const int half = eta / 2, P = 1 << (half - 1);
    auto dim = [&](const int* bits) {
        int g = 0;
        for (int i = 1; i < half; i++) g = (g << 1) | bits[i];
        int p = P - 1 - grayInverse(g);
        return bits[0] ? -v[p] : v[p];
    };
    for (int label = 0; label < M; label++) {
        int y[12], re[6], im[6];
        for (int k = 0; k < eta; k++) y[k] = (label >> (eta - 1 - k)) & 1;
        for (int i = 0; i < half; i++) { re[i] = y[2 * i + 1]; im[i] = y[2 * i]; }
        x[label] = cf32(dim(re), dim(im));
    }
    return x;
}

std::vector<int> parityMap(const Ldpc& c) {   // u(K + 360 t + s) = lambda(K + Q s + t)
    std::vector<int> m(c.n());
    for (int i = 0; i < c.n(); i++) m[i] = i;
    int Q = c.typeBQ();
    if (Q > 0)
        for (int s = 0; s < 360; s++)
            for (int t = 0; t < Q; t++) m[c.k() + 360 * t + s] = c.k() + Q * s + t;
    return m;
}


// Max-log LLRs of the cells of a block for constellations up to 256 points: the squared distance to every point, then the nearest point with
// the bit at 0 and at 1 for each bit. Written so that the compiler vectorises the distance and the minimum loops.
DECT2_SIMD_BODY void llrCellsBody(const float* pr, const float* pi, int eta, const cf32* cells, int nc, float invNv, const int* perm, float* llr) {
    const int M = 1 << eta;
    float d[256];
    for (int s = 0; s < nc; s++) {
        const float cr = cells[s].real(), ci = cells[s].imag();
        for (int m = 0; m < M; m++) { const float a = cr - pr[m], b = ci - pi[m]; d[m] = a * a + b * b; }
        for (int k = 0; k < eta; k++) {
            const int blk = M >> (k + 1);   // labels with bit k at 0 and at 1 alternate in runs of this length
            float m0 = 1e30f, m1 = 1e30f;
            if (blk >= 8) {
                float v0[8], v1[8];
                for (int j = 0; j < 8; j++) v0[j] = v1[j] = 1e30f;
                for (int base = 0; base < M; base += 2 * blk) {
                    for (int i = 0; i < blk; i += 8)
                        for (int j = 0; j < 8; j++) { v0[j] = std::min(v0[j], d[base + i + j]); v1[j] = std::min(v1[j], d[base + blk + i + j]); }
                }
                for (int j = 0; j < 8; j++) { m0 = std::min(m0, v0[j]); m1 = std::min(m1, v1[j]); }
            } else {
                for (int base = 0; base < M; base += 2 * blk)
                    for (int i = 0; i < blk; i++) { m0 = std::min(m0, d[base + i]); m1 = std::min(m1, d[base + blk + i]); }
            }
            llr[perm[s * eta + k]] = (m1 - m0) * invNv;
        }
    }
}
DECT2_AVX2_DISPATCH(void, llrCells, (const float* pr, const float* pi, int eta, const cf32* cells, int nc, float invNv, const int* perm, float* llr), (pr, pi, eta, cells, nc, invNv, perm, llr))

// The same for constellations built from one quadrant (labels: the two top bits say which quadrant, the rest which point of it, w -> -conj(w) ->
// conj(w) -> -w): the cell is folded into the first quadrant and only the 2^(eta-2) points of that quadrant are measured. The bits of the point
// come from the nearest points of the bit's two classes inside the quadrant; the two quadrant bits (sign of the imaginary and of the real part)
// from the nearest point of the quadrant and of its mirror image across the axis in question. Points of the quadrant across both axes are
// ignored: they are never nearer than the mirror image across one axis.
DECT2_SIMD_BODY void llrCellsQuadBody(const float* qa, const float* qb, int eta, const cf32* cells, int nc, float invNv, const int* perm, float* llr) {
    const int B = 1 << (eta - 2);
    float d0[64], d1[64], d2[64];
    for (int s = 0; s < nc; s++) {
        const float cr = cells[s].real(), ci = cells[s].imag();
        const float ar = cr < 0 ? -cr : cr, ai = ci < 0 ? -ci : ci;
        for (int i = 0; i < B; i++) {
            const float dr = ar - qa[i], dR = ar + qa[i], di = ai - qb[i], dI = ai + qb[i];
            d0[i] = dr * dr + di * di;
            d1[i] = dR * dR + di * di;   // mirror image across the imaginary axis (real part flipped)
            d2[i] = dr * dr + dI * dI;   // across the real axis
        }
        float own = 1e30f, flipRe = 1e30f, flipIm = 1e30f;
        for (int i = 0; i < B; i++) { own = std::min(own, d0[i]); flipRe = std::min(flipRe, d1[i]); flipIm = std::min(flipIm, d2[i]); }
        // y0: sign of the imaginary part (1 = negative), y1: of the real part
        const float l0 = ci < 0 ? (own - flipIm) : (flipIm - own), l1 = cr < 0 ? (own - flipRe) : (flipRe - own);
        llr[perm[s * eta + 0]] = l0 * invNv;
        llr[perm[s * eta + 1]] = l1 * invNv;
        for (int k = 2; k < eta; k++) {
            const int blk = B >> (k - 1);   // points whose bit k is 0 and 1 alternate in runs of this length
            float m0 = 1e30f, m1 = 1e30f;
            if (blk >= 8) {
                float v0[8], v1[8];
                for (int j = 0; j < 8; j++) v0[j] = v1[j] = 1e30f;
                for (int base = 0; base < B; base += 2 * blk)
                    for (int i = 0; i < blk; i += 8)
                        for (int j = 0; j < 8; j++) { v0[j] = std::min(v0[j], d0[base + i + j]); v1[j] = std::min(v1[j], d0[base + blk + i + j]); }
                for (int j = 0; j < 8; j++) { m0 = std::min(m0, v0[j]); m1 = std::min(m1, v1[j]); }
            } else {
                for (int base = 0; base < B; base += 2 * blk)
                    for (int i = 0; i < blk; i++) { m0 = std::min(m0, d0[base + i]); m1 = std::min(m1, d0[base + blk + i]); }
            }
            llr[perm[s * eta + k]] = (m1 - m0) * invNv;
        }
    }
}
DECT2_AVX2_DISPATCH(void, llrCellsQuad, (const float* qa, const float* qb, int eta, const cf32* cells, int nc, float invNv, const int* perm, float* llr), (qa, qb, eta, cells, nc, invNv, perm, llr))

} // namespace

int blockInterleaverType(int n, int eta, int rate15) {
    const char* row = n == 64800 ? (eta >= 2 && eta <= 12 ? kTypes64[modIndex(eta)] : nullptr) : (eta >= 2 && eta <= 8 ? kTypes16[modIndex(eta)] : nullptr);
    if (!row || rate15 < 2 || rate15 > 13) return -1;
    return row[rate15 - 2] == 'A' ? 0 : 1;
}

std::vector<int> blockInterleaverMap(int n, int eta, int type) {
    std::vector<int> m(n);
    if (type == 0) {
        static const int nr1_64[6] = {32400, 16200, 10800, 7920, 6480, 5400}, nr2_64[6] = {0, 0, 0, 180, 0, 0};
        static const int nr1_16[4] = {7920, 3960, 2520, 1800}, nr2_16[4] = {180, 90, 180, 225};
        int mi = modIndex(eta);
        int nr1 = n == 64800 ? nr1_64[mi] : nr1_16[mi], nr2 = n == 64800 ? nr2_64[mi] : nr2_16[mi];
        for (int j = 0; j < n; j++) {
            int r = j / eta, c = j % eta;
            m[j] = r < nr1 ? c * nr1 + r : eta * nr1 + c * nr2 + (r - nr1);
        }
    } else {
        static const int p1_64[6] = {64800, 64800, 64800, 63360, 64800, 64800}, p1_16[4] = {15840, 15840, 15120, 14400};
        int mi = modIndex(eta);
        int part1 = n == 64800 ? p1_64[mi] : p1_16[mi];
        for (int j = 0; j < n; j++) {
            if (j < part1) {
                int set = j / (eta * 360), within = j % (eta * 360);
                int col = within / eta, row = within % eta;
                m[j] = set * eta * 360 + row * 360 + col;
            } else m[j] = j;
        }
    }
    return m;
}

Bicm::Bicm(const BicmConfig& c) : cfg_(c) {
    const int n = c.nInner, eta = c.bitsPerCell;
    if ((n != 16200 && n != 64800) || c.rate15 < 2 || c.rate15 > 13 || eta < 2 || eta > 12 || eta % 2) return;
    if (n == 16200 && eta > 8) return;
    const Ldpc& ldpc = ldpcCode(n, c.rate15);
    if (!ldpc.ok()) return;
    kLdpc_ = ldpc.k();
    const int mouter = c.outer == 0 ? (n == 64800 ? 192 : 168) : c.outer == 1 ? 32 : 0;
    kPayload_ = kLdpc_ - mouter;
    points_ = buildPoints(eta, c.rate15);
    if (points_.empty()) return;
    if (eta >= 2 && eta <= 8) {   // one quadrant and its reflections?
        const int B = 1 << (eta - 2);
        bool q = true;
        for (int i = 0; i < B && q; i++) {
            const cf32 w = points_[(size_t)i];
            q = w.real() > 0 && w.imag() > 0 && std::abs(points_[(size_t)(B + i)] - (-std::conj(w))) < 1e-6f && std::abs(points_[(size_t)(2 * B + i)] - std::conj(w)) < 1e-6f && std::abs(points_[(size_t)(3 * B + i)] + w) < 1e-6f;
        }
        if (q) { quad_ = true; for (int i = 0; i < B; i++) { qa_.push_back(points_[(size_t)i].real()); qb_.push_back(points_[(size_t)i].imag()); } }
    }
    int type = blockInterleaverType(n, eta, c.rate15);
    const unsigned short* pi = groupPermutation(n, eta, c.rate15);
    if (type < 0 || !pi) return;
    auto m1 = parityMap(ldpc);
    auto m3 = blockInterleaverMap(n, eta, type);
    perm_.resize(n);
    for (int j = 0; j < n; j++) {
        int v = m3[j];                         // position in the group-wise interleaved word
        int grp = v / 360, off = v % 360;
        int u = pi[grp] * 360 + off;           // position in the parity-interleaved word
        perm_[j] = m1[u];                      // bit of the LDPC code word
    }
    ok_ = true;
}

std::vector<cf32> Bicm::encode(const std::vector<uint8_t>& payload) const {
    if (!ok_ || (int)payload.size() != kPayload_) return {};
    std::vector<uint8_t> bits(payload);
    if (cfg_.outer == 0) {
        const BchCode bch(cfg_.nInner == 16200, 12);
        bch.encode(bits, kPayload_);
    } else if (cfg_.outer == 1) {
        uint32_t crc = l1Crc32(bits.data(), (int)bits.size());
        for (int i = 31; i >= 0; i--) bits.push_back((crc >> i) & 1);
    }
    ldpcCode(cfg_.nInner, cfg_.rate15).encode(bits);
    const int eta = cfg_.bitsPerCell, nc = cells();
    std::vector<cf32> out(nc);
    for (int s = 0; s < nc; s++) {
        int label = 0;
        for (int k = 0; k < eta; k++) label = (label << 1) | bits[perm_[s * eta + k]];
        out[s] = points_[label];
    }
    return out;
}

void Bicm::llrs(const cf32* cells, float noiseVar, std::vector<float>& llr, const float* w) const {
    DECT2_STAGE("a3.demap");
    const int eta = cfg_.bitsPerCell, nc = this->cells(), M = 1 << eta;
    const float nv = std::max(noiseVar, 1e-5f);
    llr.assign((size_t)cfg_.nInner, 0.f);
    if (eta <= 8) {
        std::vector<float> pr((size_t)M), pi((size_t)M);
        for (int m = 0; m < M; m++) { pr[(size_t)m] = points_[(size_t)m].real(); pi[(size_t)m] = points_[(size_t)m].imag(); }
        if (quad_) llrCellsQuad(qa_.data(), qb_.data(), eta, cells, nc, 1.f / nv, perm_.data(), llr.data());
        else llrCells(pr.data(), pi.data(), eta, cells, nc, 1.f / nv, perm_.data(), llr.data());
    } else {
        // separable: one dimension at a time over the 2^(eta/2) levels of that dimension
        const int half = eta / 2, L = 1 << half;
        std::vector<float> lev(L);
        // levels of the real dimension for each half-label (bits y1, y3, ...): taken from points whose imaginary label bits are 0
        std::vector<float> levRe(L), levIm(L);
        for (int hl = 0; hl < L; hl++) {
            int labRe = 0, labIm = 0;
            for (int i = 0; i < half; i++) {
                int b = (hl >> (half - 1 - i)) & 1;
                labRe |= b << (eta - 1 - (2 * i + 1));
                labIm |= b << (eta - 1 - (2 * i));
            }
            levRe[hl] = points_[labRe].real();
            levIm[hl] = points_[labIm].imag();
        }
        for (int s = 0; s < nc; s++) {
            for (int dimI = 0; dimI < 2; dimI++) {
                float z = dimI == 0 ? cells[s].real() : cells[s].imag();
                const std::vector<float>& lv = dimI == 0 ? levRe : levIm;
                float best0[6], best1[6];
                for (int i = 0; i < half; i++) best0[i] = best1[i] = 1e30f;
                for (int hl = 0; hl < L; hl++) {
                    float d = (z - lv[hl]) * (z - lv[hl]);
                    for (int i = 0; i < half; i++) {
                        if ((hl >> (half - 1 - i)) & 1) best1[i] = std::min(best1[i], d); else best0[i] = std::min(best0[i], d);
                    }
                }
                for (int i = 0; i < half; i++) {
                    int k = 2 * i + (dimI == 0 ? 1 : 0);   // real part: y1, y3, ...; imaginary part: y0, y2, ...
                    llr[perm_[s * eta + k]] = (best1[i] - best0[i]) / nv;
                }
            }
        }
    }
    // the channel state of each cell: a cell in a fade carries less, one on a strong carrier more (the soft bits of noise noiseVar / w)
    if (w) for (int s = 0; s < nc; s++) { const float g = w[s]; if (g == 1.f) continue; for (int k = 0; k < eta; k++) llr[(size_t)perm_[(size_t)(s * eta + k)]] *= g; }
    // a few confidently wrong values (a cell the equaliser got wrong, noise the estimate left out) are as strong as many right ones for the min-sum
    // decoder and keep it from converging although the rest is clean: no value counts for more than this
    for (auto& v : llr) v = std::max(-kLlrLimit, std::min(kLlrLimit, v));
}

bool Bicm::finish(const std::vector<uint8_t>& hard, std::vector<uint8_t>& payload) const {
    DECT2_STAGE("a3.bch");
    std::vector<uint8_t> bits(hard.begin(), hard.begin() + kLdpc_);
    if (cfg_.outer == 0) {
        const BchCode bch(cfg_.nInner == 16200, 12);
        if (bch.decode(bits) < 0) return false;
        bits.resize(kPayload_);
    } else if (cfg_.outer == 1) {
        uint32_t want = l1Crc32(bits.data(), kPayload_), got = 0;
        for (int i = kPayload_; i < kLdpc_; i++) got = (got << 1) | bits[i];
        if (want != got) return false;
        bits.resize(kPayload_);
    }
    payload.swap(bits);
    return true;
}

bool Bicm::decode(const cf32* cells, float noiseVar, std::vector<uint8_t>& payload, int* iters) const {
    if (!ok_) return false;
    std::vector<float> llr;
    llrs(cells, noiseVar, llr);
    std::vector<uint8_t> hard;
    if (!decodeLlr(llr, hard, iters)) return false;
    return finish(hard, payload);
}

// One code word from float LLRs. Type B codes on the quasi-cyclic decoder of dect2::LdpcCode with the two-minimum correction (8-bit first for the
// high rates of the small constellations, which falls back to float itself); Type A on the general decoder, also with the correction. Against the
// layered 8-bit offset min-sum and the plain float fallback this gains 0.2 to 0.4 dB (16200 rate 8/15 at 1.4 dB on an ideal channel: 1 failed word
// of 30 instead of 29) and runs far faster than the general float decoder.
bool Bicm::decodeLlr(const std::vector<float>& llr, std::vector<uint8_t>& hard, int* iters) const {
    DECT2_STAGE("a3.ldpc");
    if (const dect2::LdpcCode* qc = qcCode(cfg_.nInner, cfg_.rate15)) {
        const float a = qcAlpha(cfg_.nInner, cfg_.rate15);
        int it = 0;
        const bool ok = cfg_.rate15 >= 10 && cfg_.bitsPerCell <= 4 ? qc->decodeFast8(llr, 50, hard, &it, a, true) : qc->decodeFast(llr, 50, hard, &it, a, true);
        if (iters) *iters = it;
        return ok;
    }
    return ldpcCode(cfg_.nInner, cfg_.rate15).decode(llr, 60, hard, iters, typeAAlpha(cfg_.nInner, cfg_.rate15), true);
}

void Bicm::decodeBatch(const std::vector<const cf32*>& cells, float noiseVar, std::vector<std::vector<uint8_t>>& payload, std::vector<char>& ok,
                       const std::vector<const float*>* weights) const {
    const int nb = (int)cells.size();
    auto wOf = [&](int b) -> const float* { return weights && (size_t)b < weights->size() ? (*weights)[(size_t)b] : nullptr; };
    payload.assign((size_t)nb, std::vector<uint8_t>());
    ok.assign((size_t)nb, 0);
    if (!ok_ || nb == 0 || nb > Ldpc::kBatch) return;
    if (const dect2::LdpcCode* qc = qcCode(cfg_.nInner, cfg_.rate15)) {
        // Type B: on the GPU in one batch when it is on and worth it (gpu_policy.h; the words it fails on go to the CPU decoder below), else word by word
        // on the quasi-cyclic decoder (decodeLlr)
        std::vector<float> llr;
        std::vector<uint8_t> hard;
        std::vector<uint8_t> gHard, gOk;
        const int mode = gpupol::liveMode(gpupol::Atsc3);
        static const bool noGpu = getenv("DECT2_NOGPU") != nullptr;
        // Auto: a batch of 12 or more words (a GPU round trip is a fixed cost; docs/GPU-AUDIT-3.7.md has the measured crossover)
        const bool wantGpu = !noGpu && mode != 0 && (mode == 1 || nb >= 12) && GpuLdpc::instance().available();
        if (wantGpu) {
            std::vector<float> all((size_t)nb * (size_t)cfg_.nInner);
            for (int b = 0; b < nb; b++) {
                llrs(cells[(size_t)b], noiseVar, llr, wOf(b));
                if (llr.size() != (size_t)cfg_.nInner) { all.clear(); break; }
                std::copy(llr.begin(), llr.end(), all.begin() + (size_t)b * (size_t)cfg_.nInner);
            }
            std::vector<int> it((size_t)nb);
            gHard.resize((size_t)nb * (size_t)cfg_.nInner); gOk.resize((size_t)nb);
            if (!all.empty() && GpuLdpc::instance().decode(*qc, all.data(), nb, 50, gHard.data(), gOk.data(), it.data(), qcAlpha(cfg_.nInner, cfg_.rate15), true)) gpupol::reportGpu(gpupol::Atsc3, true);
            else { gOk.clear(); gpupol::reportGpu(gpupol::Atsc3, false); }
        } else gpupol::reportGpu(gpupol::Atsc3, false);
        for (int b = 0; b < nb; b++) {
            if (!gOk.empty() && gOk[(size_t)b]) {
                hard.assign(gHard.begin() + (size_t)b * (size_t)cfg_.nInner, gHard.begin() + (size_t)(b + 1) * (size_t)cfg_.nInner);
                if (finish(hard, payload[(size_t)b])) { ok[(size_t)b] = 1; continue; }
            }
            llrs(cells[(size_t)b], noiseVar, llr, wOf(b));   // the CPU decoder: no GPU, or the GPU did not converge on this word (it also has the 8-bit first pass)
            ok[(size_t)b] = decodeLlr(llr, hard) && finish(hard, payload[(size_t)b]);
        }
        return;
    }
    std::vector<std::vector<int8_t>> q((size_t)nb);
    std::vector<const int8_t*> ptr((size_t)nb);
    std::vector<float> llr;
    for (int b = 0; b < nb; b++) {
        llrs(cells[(size_t)b], noiseVar, llr, wOf(b));
        q[(size_t)b].resize(llr.size());
        for (size_t i = 0; i < llr.size(); i++) q[(size_t)b][i] = (int8_t)std::lround(llr[i]);
        ptr[(size_t)b] = q[(size_t)b].data();
    }
    std::vector<std::vector<uint8_t>> hard;
    std::vector<char> good;
    const Ldpc& code = ldpcCode(cfg_.nInner, cfg_.rate15);
    // the values are the limited LLRs rounded to integers (+-20): with the 8-bit range filled to +-31 the decoder failed on 1.5% of the blocks, which
    // the float decoder then had to take over
    { DECT2_STAGE("a3.ldpc"); code.decodeBatch(ptr, 20, hard, good); }
    // A block the 8-bit decoder gave up on may still decode with the float one (60 iterations). When most blocks of the batch fail it is a damaged
    // frame (a gap in the samples): the float decoder would spend about 150 ms on each and keep the whole receiver waiting for many seconds.
    int failed = 0;
    for (int b = 0; b < nb; b++) failed += !good[(size_t)b];
    const bool retry = failed * 4 <= nb;
    for (int b = 0; b < nb; b++) {
        if (!good[(size_t)b] && retry) {   // the 8-bit decoder gave up: the float one has the whole 60 iterations
            llrs(cells[(size_t)b], noiseVar, llr);
            good[(size_t)b] = decodeLlr(llr, hard[(size_t)b]);
        }
        ok[(size_t)b] = good[(size_t)b] && finish(hard[(size_t)b], payload[(size_t)b]);
    }
}

} // namespace atsc3
} // namespace dect2
