#include "dect2/atsc3_bicm.h"
#include "atsc3_data_tables.h"
#include "atsc3_ldpc.h"
#include "dect2/atsc3_l1.h"
#include "dect2/t2fec.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>

namespace dect2 {
namespace atsc3 {

namespace {

// Table 6.8 and 6.9: block interleaver type (0 = A, 1 = B), rates 2/15 .. 13/15
const char* kTypes64[6] = {"AAAAAAAAAAAA", "AAABAABBAAAA", "AAAAABABBAAB", "AAABBBBABBAB", "AAABABABBBAA", "AAAAABAAAAAA"};
const char* kTypes16[4] = {"AAAABBABAAAA", "AAAABBABABAB", "AAAABBABAAAA", "AAAABAAAABAA"};

int modIndex(int eta) { return eta / 2 - 1; }   // 2, 4, 6, 8, 10, 12 -> 0..5

int grayInverse(int g) { int q = 0; for (; g; g >>= 1) q ^= g; return q; }

std::vector<cf32> buildPoints(int eta, int rate15) {
    if (eta == 2) {
        const float s = 0.70710678f;
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

bool Bicm::decode(const cf32* cells, float noiseVar, std::vector<uint8_t>& payload, int* iters) const {
    if (!ok_) return false;
    const int eta = cfg_.bitsPerCell, nc = this->cells(), M = 1 << eta;
    const float nv = std::max(noiseVar, 1e-5f);
    std::vector<float> llr(cfg_.nInner, 0.f);
    if (eta <= 8) {
        for (int s = 0; s < nc; s++) {
            float best0[8], best1[8];
            for (int k = 0; k < eta; k++) best0[k] = best1[k] = 1e30f;
            for (int lab = 0; lab < M; lab++) {
                float d = std::norm(cells[s] - points_[lab]);
                for (int k = 0; k < eta; k++) {
                    if ((lab >> (eta - 1 - k)) & 1) best1[k] = std::min(best1[k], d); else best0[k] = std::min(best0[k], d);
                }
            }
            for (int k = 0; k < eta; k++) llr[perm_[s * eta + k]] = (best1[k] - best0[k]) / nv;
        }
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
    std::vector<uint8_t> hard;
    if (!ldpcCode(cfg_.nInner, cfg_.rate15).decode(llr, 60, hard, iters)) return false;
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

} // namespace atsc3
} // namespace dect2
