// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/time/wwvb_phase.h"
#include <cmath>
#include <cstdlib>

namespace dect2 {

namespace {
constexpr int kFs = 8000, kBufSec = 14;
const int kSyncT[13] = {0, 0, 1, 1, 1, 0, 1, 1, 0, 1, 0, 0, 0};
const int kPar[5][15] = {{23, 21, 20, 17, 16, 15, 14, 13, 9, 8, 6, 5, 4, 2, 0}, {24, 22, 21, 18, 17, 16, 15, 14, 10, 9, 7, 6, 5, 3, 1}, {25, 23, 22, 19, 18, 17, 16, 15, 11, 10, 8, 7, 6, 4, 2},
                         {24, 21, 19, 18, 15, 14, 13, 12, 11, 7, 6, 4, 3, 2, 0}, {25, 22, 20, 19, 16, 15, 14, 13, 12, 8, 7, 5, 4, 3, 1}};
// table 4: code word -> dst_on, leap_sec (dst_ls[4..0] as a number)
const struct { int w, dst, leap; } kDst[12] = {{0x05, 0, 0}, {0x12, 0, 0}, {0x13, 1, 0}, {0x0A, 1, 0}, {0x1A, 3, 0}, {0x08, 2, 0}, {0x1B, 3, 1}, {0x07, 2, 1}, {0x19, 2, 1}, {0x0D, 3, 1}, {0x1F, 2, 3}, {0x0E, 2, 3}};
int parity(uint32_t t, int r) { int v = 0; for (int i = 0; i < 15; i++) v ^= (t >> kPar[r][i]) & 1; return v; }
void timeBits(uint32_t t, uint8_t* f) {   // positions 13-18, 20-28, 30-38, 40-46 and the copy of bit 0 at 19
    uint32_t w = 0;
    for (int r = 4; r >= 0; r--) w = w << 1 | (uint32_t)parity(t, r);
    uint64_t all = ((uint64_t)w << 26) | t;
    static const int pos[31] = {13, 14, 15, 16, 17, 18, 20, 21, 22, 23, 24, 25, 26, 27, 28, 30, 31, 32, 33, 34, 35, 36, 37, 38, 40, 41, 42, 43, 44, 45, 46};
    for (int i = 0; i < 31; i++) f[pos[i]] = (all >> (30 - i)) & 1;
    f[19] = t & 1;
}
}

std::array<uint8_t, 60> WwvbPhase::frameBits(int64_t utcMinute) {
    std::array<uint8_t, 60> f{}; f.fill(0);
    for (int i = 0; i < 13; i++) f[(size_t)i] = (uint8_t)kSyncT[i];
    const uint32_t t = (uint32_t)((utcMinute - 946684800) / 60);
    timeBits(t, f.data());
    f[47] = 0; f[48] = 0; f[50] = 1; f[51] = 0; f[52] = 1;   // dst_ls = 0b00101: no DST, no leap second (table 4, row 1)
    f[59] = (uint8_t)kSyncT[0];
    return f;
}

WwvbPhase::WwvbPhase() { reset(); }
void WwvbPhase::reset() { buf_.assign((size_t)kFs * kBufSec, std::complex<float>(0, 0)); n_ = 0; align_ = -1; sinceSearch_ = 0; bits_.clear(); z_.clear(); st_ = WwvbPhaseStatus(); fPerSec_ = 0; lastT_ = 0; haveLast_ = false; }

void WwvbPhase::feed(std::complex<float> z) {
    buf_[n_ % buf_.size()] = z;
    n_++;
    if (align_ < 0) {
        if (n_ >= buf_.size() && (n_ % (kFs * 2)) == 0) searchAlignment();
        return;
    }
    if (((n_ + (uint64_t)kFs - (uint64_t)align_) % kFs) == 0) newSecond();
}

void WwvbPhase::searchAlignment() {
    // integrating inside a bit is coherent; across a bit change it is not: the best offset of the bit boundary maximises the summed magnitudes of whole seconds
    double best = -1; int bo = 0;
    auto metric = [&](int o) {
        double acc = 0;
        for (int s = 0; s < kBufSec - 1; s++) {
            std::complex<double> z(0, 0);
            for (int i = 0; i < kFs; i++) z += std::complex<double>(buf_[(size_t)(((uint64_t)o + (uint64_t)s * kFs + (uint64_t)i + n_) % buf_.size())]);
            acc += std::abs(z);
        }
        return acc;
    };
    for (int o = 0; o < kFs; o += 200) { const double m = metric(o); if (m > best) { best = m; bo = o; } }
    for (int o = bo - 180; o <= bo + 180; o += 20) { int q = ((o % kFs) + kFs) % kFs; const double m = metric(q); if (m > best) { best = m; bo = q; } }
    // the offset is relative to the oldest sample in the buffer: the oldest sample is n_ - buf size
    align_ = (int)((n_ + (uint64_t)bo) % kFs);
    sinceSearch_ = 0;
}

void WwvbPhase::newSecond() {
    std::complex<float> z(0, 0);
    for (int i = 1; i <= kFs; i++) z += buf_[(size_t)((n_ - (uint64_t)i) % buf_.size())];
    z_.push_back(z);
    if (z_.size() > 60 + 30) z_.erase(z_.begin());
    tryFrame();
    if (++sinceSearch_ >= 900) { align_ = -1; }                   // now and then look for the boundary again (clock drift of a long run)
}

void WwvbPhase::tryFrame() {
    const size_t N = z_.size();
    if (N < 60) return;
    // the carrier's remaining frequency, from the rotation of the squared symbols; then the phase from a sliding window of them, which removes the data
    {
        std::complex<double> rot(0, 0);
        for (size_t i = 1; i < N; i++) { const std::complex<double> a(z_[i]), c(z_[i - 1]); rot += a * a * std::conj(c * c); }
        fPerSec_ = 0.5 * std::arg(rot);
    }
    std::vector<int> b(N);
    const int W = 8;
    double prev = 0; bool have = false;
    for (size_t i = 0; i < N; i++) {
        std::complex<double> s(0, 0);
        for (size_t j = i > (size_t)W ? i - W : 0; j < std::min(N, i + (size_t)W + 1); j++) { const std::complex<double> v = std::complex<double>(z_[j]) * std::polar(1.0, -fPerSec_ * ((double)j - (double)i)); s += v * v; }
        double ph = 0.5 * std::arg(s);
        if (have) { while (ph - prev > M_PI / 2) ph -= M_PI; while (ph - prev < -M_PI / 2) ph += M_PI; }
        prev = ph; have = true;
        b[i] = (std::complex<double>(z_[i]) * std::polar(1.0, -ph)).real() < 0 ? 1 : 0;
    }
    // the sync word at the start of the last 60 symbols, in either polarity
    const size_t s0 = N - 60;
    for (int pol = 0; pol < 2; pol++) {
        int errs = 0;
        for (int i = 0; i < 13; i++) errs += (b[s0 + (size_t)i] ^ pol) != kSyncT[i];
        if (errs > 1) continue;
        uint8_t f[60]; for (int i = 0; i < 60; i++) f[i] = (uint8_t)(b[s0 + (size_t)i] ^ pol);
        // decode: parity then time
        static const int pos[31] = {13, 14, 15, 16, 17, 18, 20, 21, 22, 23, 24, 25, 26, 27, 28, 30, 31, 32, 33, 34, 35, 36, 37, 38, 40, 41, 42, 43, 44, 45, 46};
        uint64_t all = 0; for (int i = 0; i < 31; i++) all = all << 1 | f[pos[i]];
        uint32_t t = (uint32_t)(all & 0x3FFFFFF); int rx = (int)(all >> 26);
        int syn = 0; for (int r = 0; r < 5; r++) syn |= (parity(t, r) ^ ((rx >> r) & 1)) << r;
        int corrected = 0;
        if (syn) {
            int fix = -1;
            for (int k = 0; k < 26 && fix < 0; k++) { int v = 0; for (int r = 0; r < 5; r++) for (int i = 0; i < 15; i++) if (kPar[r][i] == k) v |= 1 << r; if (v == syn) fix = k; }
            if (fix >= 0) { t ^= 1u << fix; corrected = 1; }
            else { bool one = false; for (int r = 0; r < 5; r++) if (syn == (1 << r)) one = true; if (one) corrected = 1; else { st_.framesBad++; continue; } }
        }
        if ((int)(t & 1) != f[19] && corrected == 0) { /* the copy of bit 0 disagrees, a soft hint only */ }
        if (corrected && (int)(t & 1) != f[19]) { st_.framesBad++; continue; }
        const bool follows = haveLast_ && t == lastT_ + 1;
        lastT_ = t; haveLast_ = true;
        if (!follows && !st_.locked) return;                     // two frames in a row before the first time is believed
        st_.locked = true; st_.corrected = corrected; st_.framesOk++;
        st_.utcMinute = 946684800 + (int64_t)t * 60;
        int w = f[47] << 4 | f[48] << 3 | f[50] << 2 | f[51] << 1 | f[52];
        st_.dstOn = -1; st_.leap = -1;
        for (auto& e : kDst) if (e.w == w) { st_.dstOn = e.dst; st_.leap = e.leap; }
        return;
    }
}

}  // namespace dect2
