// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi OFDM PHY: tables, coding and the transmit half used for the test signal (see wifi_ofdm.h). Clause numbers: IEEE 802.11-2020.
#include "dect2/wifi/wifi_ofdm.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/wifi/wifi_proto.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2::wifi::ofdm {

namespace {
// Long training sequence L(-26..26), Equation 17-8 (the centre entry is the unused DC carrier)
const int8_t kLtf[53] = {1, 1, -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1, 1, 1, -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1, 0,
                         1, -1, -1, 1, 1, -1, 1, -1, 1, -1, -1, -1, -1, -1, 1, 1, -1, -1, 1, -1, 1, -1, 1, 1, 1, 1};
// Short training sequence S(-26..26) (Equation 17-6) as (re, im) signs times sqrt(13/6): the 12 non-zero carriers are +-(1 + j)
const int8_t kStf[53][2] = {
    {0,0},{0,0},{1,1},{0,0},{0,0},{0,0},{-1,-1},{0,0},{0,0},{0,0},{1,1},{0,0},{0,0},{0,0},{-1,-1},{0,0},{0,0},{0,0},{-1,-1},{0,0},{0,0},{0,0},{1,1},{0,0},{0,0},{0,0},
    {0,0},
    {0,0},{0,0},{0,0},{-1,-1},{0,0},{0,0},{0,0},{-1,-1},{0,0},{0,0},{0,0},{1,1},{0,0},{0,0},{0,0},{1,1},{0,0},{0,0},{0,0},{1,1},{0,0},{0,0},{0,0},{1,1},{0,0},{0,0}};
const int kPilotK[4] = {-21, -7, 7, 21};
const int8_t kPilotBase[4] = {1, 1, 1, -1};       // Equation 17-24 (before the polarity p_n)

inline int bin(int k) { return (k + kFft) % kFft; }
inline bool isPilot(int k) { return k == -21 || k == -7 || k == 7 || k == 21; }

// the 48 data carriers in increasing frequency (Equation 17-22 mapping order)
const std::vector<int>& dataCarriers() {
    static const std::vector<int> v = [] { std::vector<int> r; for (int k = -26; k <= 26; k++) if (k && !isPilot(k)) r.push_back(k); return r; }();
    return v;
}

// 64 time samples from carrier values (unnormalised inverse FFT scaled to unit power for 52 unit carriers)
std::vector<cf32> toTime(const std::vector<cf32>& X) {
    std::vector<cf32> t(X);
    static Fft fft(kFft);
    fft.inverse(t.data());
    const float s = 1.0f / std::sqrt(52.0f);
    for (cf32& v : t) v *= s;
    return t;
}
} // namespace

// p_n: the scrambler sequence with an all-ones start, 0 -> +1, 1 -> -1 (Equation 17-25), period 127.
int pilotPolarity(int n) {
    static const std::vector<int8_t> p = [] {
        std::vector<int8_t> r(127);
        unsigned st = 0x7F;
        for (int i = 0; i < 127; i++) { const unsigned fb = (st >> 6 & 1) ^ (st >> 3 & 1); st = (st << 1 | fb) & 0x7F; r[(size_t)i] = fb ? -1 : 1; }
        return r;
    }();
    return p[(size_t)(((n % 127) + 127) % 127)];
}

// The long training symbol's 64 time samples (Equation 17-9).
const std::vector<cf32>& ltfSymbol() {
    static const std::vector<cf32> v = [] {
        std::vector<cf32> X(kFft, cf32(0, 0));
        for (int k = -26; k <= 26; k++) X[(size_t)bin(k)] = cf32((float)kLtf[k + 26], 0);
        return toTime(X);
    }();
    return v;
}

// Rate 1/2 convolutional encoder, g0 = 133, g1 = 171 octal (Figure 17-8): register bit 0 is the newest input.
std::vector<uint8_t> convEncode(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> o;
    o.reserve(in.size() * 2);
    unsigned s = 0;
    for (uint8_t b : in) {
        const unsigned reg = (b & 1u) | (s << 1);
        o.push_back((uint8_t)(__builtin_parity(reg & 0x6D)));   // g0 = 1011011: delays 0, 2, 3, 5, 6
        o.push_back((uint8_t)(__builtin_parity(reg & 0x4F)));   // g1 = 1111001: delays 0, 1, 2, 3, 6
        s = reg & 0x3F;
    }
    return o;
}

// Soft Viterbi decoder for that code, terminated in state 0 (the last 6 input bits are the zero tail).
int viterbiDecode(const float* soft, int n, uint8_t* out) {
    constexpr float kNeg = -1e30f;
    std::vector<uint64_t> dec((size_t)n);
    float m[64], nm[64];
    for (int s = 0; s < 64; s++) m[s] = kNeg;
    m[0] = 0;
    // branch outputs for (previous state, input)
    uint8_t oa[64][2], ob[64][2];
    for (int s = 0; s < 64; s++) for (int b = 0; b < 2; b++) { const unsigned reg = (unsigned)b | ((unsigned)s << 1); oa[s][b] = (uint8_t)__builtin_parity(reg & 0x6D); ob[s][b] = (uint8_t)__builtin_parity(reg & 0x4F); }
    for (int i = 0; i < n; i++) {
        // a NaN soft bit (0 / 0 from a channel estimate of zero) made every branch lose, every metric -1e30 and the int cast below UB
        // (UBSan, fuzz_engine_modes): a NaN or infinite bit counts as an erasure, and a huge finite one is held to 1e9 so the sums stay finite
        auto clean = [](float v) { return std::isfinite(v) ? std::max(-1e9f, std::min(1e9f, v)) : 0.f; };
        const float a = clean(soft[2 * i]), bb = clean(soft[2 * i + 1]);
        uint64_t d = 0;
        for (int ns = 0; ns < 64; ns++) {
            const int b = ns & 1;
            float best = kNeg; int bt = 0;
            for (int t = 0; t < 2; t++) {
                const int ps = (ns >> 1) | (t << 5);
                if (m[ps] <= kNeg) continue;
                const float v = m[ps] + (oa[ps][b] ? a : -a) + (ob[ps][b] ? bb : -bb);
                if (v > best) { best = v; bt = t; }
            }
            nm[ns] = best;
            d |= (uint64_t)bt << ns;
        }
        dec[(size_t)i] = d;
        std::memcpy(m, nm, sizeof m);
    }
    int s = 0;
    for (int i = n - 1; i >= 0; i--) {
        out[i] = (uint8_t)(s & 1);
        s = (s >> 1) | (int)(dec[(size_t)i] >> s & 1) << 5;
    }
    return m[0] > -2e9f && m[0] < 2e9f ? (int)m[0] : (m[0] > 0 ? 2000000000 : -2000000000);
}

// Scrambler / descrambler x^7 + x^4 + 1 (clause 17.3.5.5): state bit 0 is x1, bit 6 is x7; the output is x7 xor x4.
void scramble(std::vector<uint8_t>& bits, uint8_t state) {
    unsigned st = state & 0x7F;
    for (uint8_t& b : bits) {
        const unsigned fb = (st >> 6 & 1) ^ (st >> 3 & 1);
        st = (st << 1 | fb) & 0x7F;
        b ^= (uint8_t)fb;
    }
}

namespace {
// One OFDM symbol as 80 samples (cyclic prefix + 64) from 48 data bits (BPSK: 1 -> +1, 0 -> -1) after the interleaver, pilots with polarity p
void putSymbol(std::vector<cf32>& out, const uint8_t* coded48, int pol) {
    std::vector<cf32> X(kFft, cf32(0, 0));
    const auto& dc = dataCarriers();
    // interleaver, Equation 17-18 with NCBPS = 48, NBPSC = 1: input bit k goes to position 3 (k mod 16) + k / 16
    for (int k = 0; k < 48; k++) { const int j = 3 * (k % 16) + k / 16; X[(size_t)bin(dc[(size_t)j])] = cf32(coded48[k] ? 1.f : -1.f, 0); }
    for (int i = 0; i < 4; i++) X[(size_t)bin(kPilotK[i])] = cf32((float)(kPilotBase[i] * pol), 0);
    const std::vector<cf32> t = toTime(X);
    out.insert(out.end(), t.begin() + (kFft - kCp), t.end());
    out.insert(out.end(), t.begin(), t.end());
}
// The same for QPSK (12 Mbit/s): 96 coded bits, interleaved (NCBPS = 96: bit k to 6 (k mod 16) + k / 16; the second permutation does nothing
// for NBPSC = 2), each carrier's pair (b0 on I, b1 on Q) mapped to (2 b - 1) / sqrt 2 (clause 17.3.5.8)
void putSymbolQpsk(std::vector<cf32>& out, const uint8_t* coded96, int pol) {
    std::vector<cf32> X(kFft, cf32(0, 0));
    const auto& dc = dataCarriers();
    uint8_t il[96];
    for (int k = 0; k < 96; k++) il[6 * (k % 16) + k / 16] = coded96[k];
    const float a = 1.0f / std::sqrt(2.0f);
    for (int j = 0; j < 48; j++) X[(size_t)bin(dc[(size_t)j])] = cf32(il[2 * j] ? a : -a, il[2 * j + 1] ? a : -a);
    for (int i = 0; i < 4; i++) X[(size_t)bin(kPilotK[i])] = cf32((float)(kPilotBase[i] * pol), 0);
    const std::vector<cf32> t = toTime(X);
    out.insert(out.end(), t.begin() + (kFft - kCp), t.end());
    out.insert(out.end(), t.begin(), t.end());
}
} // namespace

// Preamble, SIGNAL and DATA symbols at 20 Msps (clause 17.3).
std::vector<cf32> txPpdu(const std::vector<uint8_t>& mpdu, uint8_t seed, bool qpsk) {
    std::vector<cf32> out;
    // short training: 12 carriers, period 16 samples, 160 samples in all
    std::vector<cf32> S(kFft, cf32(0, 0));
    const float sc = std::sqrt(13.0f / 6.0f);
    for (int k = -26; k <= 26; k++) S[(size_t)bin(k)] = cf32(kStf[k + 26][0] * sc, kStf[k + 26][1] * sc);
    const std::vector<cf32> st = toTime(S);
    for (int i = 0; i < kStfLen; i++) out.push_back(st[(size_t)(i % kFft)]);
    // long training: 32-sample guard (the end of the symbol), then the symbol twice
    const std::vector<cf32>& lt = ltfSymbol();
    for (int i = 32; i < kFft; i++) out.push_back(lt[(size_t)i]);
    for (int r = 0; r < 2; r++) out.insert(out.end(), lt.begin(), lt.end());
    // SIGNAL: RATE (6 Mbit/s: 1101, R1 first), reserved 0, LENGTH LSB first, even parity, six tail zeros (Figure 17-5)
    const int len = (int)mpdu.size();
    std::vector<uint8_t> sig = qpsk ? std::vector<uint8_t>{0, 1, 0, 1, 0} : std::vector<uint8_t>{1, 1, 0, 1, 0};   // 12 Mbit/s: 0101
    for (int i = 0; i < 12; i++) sig.push_back((uint8_t)(len >> i & 1));
    uint8_t par = 0;
    for (uint8_t b : sig) par ^= b;
    sig.push_back(par);
    for (int i = 0; i < 6; i++) sig.push_back(0);
    const std::vector<uint8_t> sc48 = convEncode(sig);
    putSymbol(out, sc48.data(), pilotPolarity(0));
    // DATA: SERVICE (16 zeros), PSDU LSB first, tail, pad; scrambled, then the tail bits are zero again (clause 17.3.5.3)
    std::vector<uint8_t> d(16, 0);
    for (uint8_t b : mpdu) for (int k = 0; k < 8; k++) d.push_back((uint8_t)(b >> k & 1));
    const size_t tailAt = d.size();
    const size_t dbps = qpsk ? 48 : 24;
    d.resize(((tailAt + 6 + dbps - 1) / dbps) * dbps, 0);
    scramble(d, seed ? seed : 0x5D);
    for (size_t i = 0; i < 6; i++) d[tailAt + i] = 0;
    const std::vector<uint8_t> coded = convEncode(d);
    if (qpsk) for (size_t s = 0; s * 96 < coded.size(); s++) putSymbolQpsk(out, coded.data() + s * 96, pilotPolarity((int)s + 1));
    else for (size_t s = 0; s * 48 < coded.size(); s++) putSymbol(out, coded.data() + s * 48, pilotPolarity((int)s + 1));
    return out;
}

} // namespace dect2::wifi::ofdm
