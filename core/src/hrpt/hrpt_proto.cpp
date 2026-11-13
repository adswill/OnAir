// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HRPT / AHRPT layers above the demodulator (see hrpt_proto.h).
#include "dect2/hrpt/hrpt_proto.h"
#include <cstring>
#include "dect2/common/bits.h"

namespace dect2::hrpt {

const uint16_t kNoaaSync[kNoaaSyncWords] = {0x284, 0x16F, 0x35C, 0x19D, 0x20F, 0x095};

namespace {
inline int parity7(unsigned v) { return dect2::bits::parity(v & 0xFFu); }   // folds the low 8 bits, as before
}

void CcsdsEncoder::push(int bit, uint8_t& a, uint8_t& b) {
    sr = (uint8_t)(((sr << 1) | (bit & 1)) & 0x7F);
    a = (uint8_t)parity7(sr & 0x4F);          // G1 = 171 octal, newest bit at the least significant end
    b = (uint8_t)(parity7(sr & 0x6D) ^ 1);    // G2 = 133 octal, inverted
}

void packWords(const uint16_t* w, size_t n, uint8_t* out) {
    for (size_t i = 0; i < n; i++)
        for (int k = 9; k >= 0; k--) {
            const size_t bit = i * 10 + (size_t)(9 - k);
            if (w[i] >> k & 1) out[bit >> 3] = (uint8_t)(out[bit >> 3] | 0x80 >> (bit & 7));
        }
}

void unpackWords(const uint8_t* in, size_t n, uint16_t* w) {
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        for (int k = 0; k < 10; k++) {
            const size_t bit = i * 10 + (size_t)k;
            v = v << 1 | (unsigned)(in[bit >> 3] >> (7 - (bit & 7)) & 1);
        }
        w[i] = (uint16_t)v;
    }
}

bool decodeAvhrrPacket(const std::vector<uint8_t>& u, Line& out) {
    if ((int)u.size() < kAvhrrUserBytes) return false;
    out.day = (uint32_t)(u[0] << 8 | u[1]);
    out.ms = (uint32_t)u[2] << 24 | (uint32_t)u[3] << 16 | (uint32_t)u[4] << 8 | u[5];
    const size_t nw = (size_t)(kAvhrrUserBytes - kAvhrrDataByte) * 8 / 10;
    std::vector<uint16_t> w(nw);
    unpackWords(&u[kAvhrrDataByte], nw, w.data());
    for (int x = 0; x < kHrptWidth; x++)
        for (int c = 0; c < kHrptChannels; c++) out.px[(size_t)c * kHrptWidth + (size_t)x] = w[(size_t)(kAvhrrFirstWord + x * kHrptChannels + c)];
    return true;
}

std::vector<uint8_t> encodeAvhrrPacket(const Line& l) {
    std::vector<uint8_t> u(kAvhrrUserBytes, 0);
    u[0] = (uint8_t)(l.day >> 8); u[1] = (uint8_t)l.day;
    u[2] = (uint8_t)(l.ms >> 24); u[3] = (uint8_t)(l.ms >> 16); u[4] = (uint8_t)(l.ms >> 8); u[5] = (uint8_t)l.ms;
    const size_t nw = (size_t)(kAvhrrUserBytes - kAvhrrDataByte) * 8 / 10;
    std::vector<uint16_t> w(nw, 0);
    for (int x = 0; x < kHrptWidth; x++)
        for (int c = 0; c < kHrptChannels; c++) w[(size_t)(kAvhrrFirstWord + x * kHrptChannels + c)] = l.px[(size_t)c * kHrptWidth + (size_t)x] & 0x3FF;
    packWords(w.data(), nw, &u[kAvhrrDataByte]);
    return u;
}

uint64_t noaaSync60() {
    uint64_t v = 0;
    for (int i = 0; i < kNoaaSyncWords; i++) v = v << 10 | kNoaaSync[i];
    return v;
}

bool decodeNoaaFrame(const uint16_t w[kNoaaWords], Line& out) {
    int err = 0;
    for (int i = 0; i < kNoaaSyncWords; i++) err += __builtin_popcount((unsigned)(w[i] ^ kNoaaSync[i]) & 0x3FF);
    if (err > 12) return false;
    out.day = (uint32_t)(w[8] >> 1 & 0x1FF);
    out.ms = (uint32_t)(w[9] & 0x7F) << 20 | (uint32_t)(w[10] & 0x3FF) << 10 | (uint32_t)(w[11] & 0x3FF);
    for (int x = 0; x < kHrptWidth; x++)
        for (int c = 0; c < kHrptChannels; c++) out.px[(size_t)c * kHrptWidth + (size_t)x] = w[kNoaaImageWord + x * kHrptChannels + c] & 0x3FF;
    return true;
}

void encodeNoaaFrame(const Line& l, uint16_t w[kNoaaWords]) {
    std::memset(w, 0, sizeof(uint16_t) * kNoaaWords);
    for (int i = 0; i < kNoaaSyncWords; i++) w[i] = kNoaaSync[i];
    w[8] = (uint16_t)((l.day & 0x1FF) << 1);
    w[9] = (uint16_t)(l.ms >> 20 & 0x7F);
    w[10] = (uint16_t)(l.ms >> 10 & 0x3FF);
    w[11] = (uint16_t)(l.ms & 0x3FF);
    for (int x = 0; x < kHrptWidth; x++)
        for (int c = 0; c < kHrptChannels; c++) w[kNoaaImageWord + x * kHrptChannels + c] = l.px[(size_t)c * kHrptWidth + (size_t)x] & 0x3FF;
}

// --- Viterbi ---------------------------------------------------------------------------------------------------------------------
namespace {
constexpr int kTb = 96, kEmit = 64;
struct VitTab {
    int16_t sa[4][32], sb[4][32];   // branch signs: for butterfly j, branch k (0: p0 -> 2j, 1: p1 -> 2j, 2: p0 -> 2j+1, 3: p1 -> 2j+1)
    VitTab() {
        for (int j = 0; j < 32; j++)
            for (int k = 0; k < 4; k++) {
                const int p = j | ((k & 1) ? 32 : 0), bit = k >> 1;
                const unsigned r = (unsigned)((p << 1 | bit) & 0x7F);
                sa[k][j] = parity7(r & 0x4F) ? 1 : -1;
                sb[k][j] = parity7(r & 0x6D) ? 1 : -1;
            }
    }
};
const VitTab& vt() { static const VitTab t; return t; }
}

void Viterbi::reset() { for (auto& v : m_) v = 0; n_ = 0; sinceNorm_ = 0; out_.clear(); }

// The 32 butterflies of a step as four groups of eight int16 lanes (compiler vector extension: NEON, SSE2 or AVX where there is one, plain
// scalar code elsewhere); the integer arithmetic and the tie rule (the upper branch wins only when strictly greater) are those of the scalar loop.
typedef int16_t V8 __attribute__((vector_size(16)));

void Viterbi::push(int a, int b) {
    const VitTab& T = vt();
    const int16_t A = (int16_t)a, B = (int16_t)b;
    alignas(16) int16_t ev[32], od[32], s0[32], s1[32];
    for (int j = 0; j < 32; j += 8) {
        V8 lo, hi, sa0, sb0, sa1, sb1, sa2, sb2, sa3, sb3;
        std::memcpy(&lo, m_ + j, 16); std::memcpy(&hi, m_ + j + 32, 16);
        std::memcpy(&sa0, T.sa[0] + j, 16); std::memcpy(&sb0, T.sb[0] + j, 16);
        std::memcpy(&sa1, T.sa[1] + j, 16); std::memcpy(&sb1, T.sb[1] + j, 16);
        std::memcpy(&sa2, T.sa[2] + j, 16); std::memcpy(&sb2, T.sb[2] + j, 16);
        std::memcpy(&sa3, T.sa[3] + j, 16); std::memcpy(&sb3, T.sb[3] + j, 16);
        const V8 c0 = lo + sa0 * A + sb0 * B, c1 = hi + sa1 * A + sb1 * B;
        const V8 c2 = lo + sa2 * A + sb2 * B, c3 = hi + sa3 * A + sb3 * B;
        const V8 m0 = c1 > c0, m1 = c3 > c2;                 // -1 where the upper branch wins
        const V8 e = (m0 & c1) | (~m0 & c0), o = (m1 & c3) | (~m1 & c2);
        std::memcpy(ev + j, &e, 16); std::memcpy(od + j, &o, 16);
        std::memcpy(s0 + j, &m0, 16); std::memcpy(s1 + j, &m1, 16);
    }
    alignas(16) int16_t nm[64];
    uint8_t* d = dec_[n_++];
    for (int j = 0; j < 32; j++) {
        nm[2 * j] = ev[j]; nm[2 * j + 1] = od[j];
        d[2 * j] = (uint8_t)(s0[j] & 1); d[2 * j + 1] = (uint8_t)(s1[j] & 1);
    }
    if (++sinceNorm_ >= 32) {   // 32 steps grow a metric by at most 32 x 254
        sinceNorm_ = 0;
        const int16_t base = nm[0];
        for (int s = 0; s < 64; s++) nm[s] = (int16_t)(nm[s] - base);
    }
    for (int s = 0; s < 64; s++) m_[s] = nm[s];
    if (n_ >= kTb + kEmit) traceback(kEmit, false);
}

void Viterbi::traceback(int emit, bool all) {
    int s = 0;
    for (int i = 1; i < 64; i++) if (m_[i] > m_[s]) s = i;
    uint8_t bits[160];
    for (int t = n_ - 1; t >= 0; t--) {
        bits[t] = (uint8_t)(s & 1);
        s = (s >> 1) | (int)(dec_[t][s] << 5);
    }
    const int k = all ? n_ : emit;
    out_.insert(out_.end(), bits, bits + k);
    for (int i = k; i < n_; i++) std::memcpy(dec_[i - k], dec_[i], 64);
    n_ -= k;
}

void Viterbi::flush() { if (n_ > 0) traceback(0, true); }

} // namespace dect2::hrpt
