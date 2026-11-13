// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MPT 1327 test signal generator (see mpt1327_gen.h).
#include "dect2/mpt1327/mpt1327_gen.h"
#include <cmath>

namespace dect2::mpt1327 {

namespace {
constexpr double kPi = 3.14159265358979323846;

uint64_t put(uint64_t info, int firstBit, int count, uint32_t v) {   // info holds codeword bits 1 .. 48
    const int shift = 48 - (firstBit - 1) - count;
    const uint64_t mask = ((1ull << count) - 1) << shift;
    return (info & ~mask) | (((uint64_t)v << shift) & mask);
}

uint64_t general(int pfix, int ident1, int cat, int type, int func, uint32_t params18) {
    uint64_t i = 0;
    i = put(i, 1, 1, 1);
    i = put(i, 2, 7, (uint32_t)pfix);
    i = put(i, 9, 13, (uint32_t)ident1);
    i = put(i, 22, 1, 1);
    i = put(i, 23, 3, (uint32_t)cat);
    i = put(i, 26, 2, (uint32_t)type);
    i = put(i, 28, 3, (uint32_t)func);
    i = put(i, 31, 18, params18);
    return encode(i);
}
}  // namespace

uint64_t makeCcsc(int sys) {
    // bit 1 = 0, SYS bits 2 .. 16, CCS bits 17 .. 32, preamble bits 33 .. 48; choose CCS so that the check bits equal SYNC.
    // The check bits are affine in the information bits (the XOR of the bit-63 inversion is a constant), so solve a 16 x 16 system over GF(2).
    uint64_t base = 0;
    base = put(base, 2, 15, (uint32_t)sys);
    base = put(base, 33, 16, 0xAAAA);   // 1010101010101010
    const uint16_t c0 = checkBits(base);
    uint16_t col[16];
    for (int k = 0; k < 16; k++) col[k] = (uint16_t)(checkBits(put(base, 17, 16, 1u << (15 - k))) ^ c0);   // effect of CCS bit k (bit 17 + k)
    // gaussian elimination: find x (16 bits) with XOR over set bits of col = SYNC ^ c0
    uint32_t rows[16];   // rows[r] = bits r of check as a function of the 16 unknowns plus the right side in bit 16
    const uint16_t want = (uint16_t)(kSync ^ c0);
    for (int r = 0; r < 16; r++) {
        uint32_t row = 0;
        for (int k = 0; k < 16; k++) if ((col[k] >> r) & 1) row |= 1u << k;
        if ((want >> r) & 1) row |= 1u << 16;
        rows[r] = row;
    }
    int pivotRowOf[16];
    for (int k = 0; k < 16; k++) pivotRowOf[k] = -1;
    int next = 0;
    for (int k = 0; k < 16; k++) {
        int p = -1;
        for (int r = next; r < 16; r++) if ((rows[r] >> k) & 1) { p = r; break; }
        if (p < 0) continue;
        const uint32_t t = rows[p]; rows[p] = rows[next]; rows[next] = t;
        for (int r = 0; r < 16; r++) if (r != next && ((rows[r] >> k) & 1)) rows[r] ^= rows[next];
        pivotRowOf[k] = next++;
    }
    uint32_t ccs = 0;
    for (int k = 0; k < 16; k++) if (pivotRowOf[k] >= 0 && ((rows[pivotRowOf[k]] >> 16) & 1)) ccs |= 1u << (15 - k);
    return encode(put(base, 17, 16, ccs));
}

uint64_t makeGtc(int pfix, int ident1, int d, int chan, int ident2, int n) {
    uint64_t i = 0;
    i = put(i, 1, 1, 1);
    i = put(i, 2, 7, (uint32_t)pfix);
    i = put(i, 9, 13, (uint32_t)ident1);
    i = put(i, 22, 1, 0);
    i = put(i, 23, 1, (uint32_t)d);
    i = put(i, 24, 10, (uint32_t)chan);
    i = put(i, 34, 13, (uint32_t)ident2);
    i = put(i, 47, 2, (uint32_t)n);
    return encode(i);
}

uint64_t makeAloha(int pfix, int ident1, int func, int chan4, int wt, int m, int n) {
    const uint32_t p = ((uint32_t)chan4 << 14) | ((uint32_t)wt << 11) | ((uint32_t)m << 4) | (uint32_t)n;   // CHAN4 4, WT 3, RSVD 2, M 5, N 4
    return general(pfix, ident1, 0, 0, func, p);
}

uint64_t makeAck(int pfix, int ident1, int func, int ident2, int qual, int n) {
    const uint32_t p = ((uint32_t)ident2 << 5) | ((uint32_t)qual << 4) | (uint32_t)n;   // IDENT2 13, QUAL 1, N 4
    return general(pfix, ident1, 0, 1, func, p);
}

uint64_t makeAhy(int pfix, int ident1, int func, int params18) { return general(pfix, ident1, 0, 2, func, (uint32_t)params18); }

uint64_t makeBcast(int sysdef, int sys, int params18) {
    uint64_t i = 0;
    i = put(i, 1, 1, 1);
    i = put(i, 2, 5, (uint32_t)sysdef);
    i = put(i, 7, 15, (uint32_t)sys);
    i = put(i, 22, 1, 1);
    i = put(i, 23, 3, 0);
    i = put(i, 26, 2, 3);
    i = put(i, 28, 3, 4);
    i = put(i, 31, 18, (uint32_t)params18);
    return encode(i);
}

uint64_t makeBcastChannel(int sysdef, int sys, int chan, int adjsite) {
    uint32_t p = (uint32_t)chan << 8;   // CHAN 10 bits, then 8 bits: SPARE/RSVD, and ADJSITE in the last 4 for sysdef 4
    if (sysdef == 4) p |= (uint32_t)adjsite & 15u;
    return makeBcast(sysdef, sys, (int)p);
}

uint64_t makeData(uint64_t info47) { return encode(info47 & 0x7FFFFFFFFFFFull); }

std::vector<uint8_t> controlChannelBits(int sys, const std::vector<uint64_t>& addr, int preamble) {
    std::vector<uint8_t> b;
    auto word = [&](uint64_t v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((uint8_t)((v >> i) & 1)); };
    for (int i = 0; i < preamble; i++) b.push_back((uint8_t)((i & 1) ^ 1));   // 1010 ... 10: ends with a zero
    word(kSync, 16);
    bool first = true;
    const uint64_t ccsc = makeCcsc(sys);
    for (uint64_t a : addr) {
        if (!first) word(ccsc, 64);
        first = false;
        word(a, 64);
    }
    // two trailing slots keep the stream going so the last codeword is completely received
    word(ccsc, 64);
    return b;
}

std::vector<float> ffskAudio(const std::vector<uint8_t>& bits, double fs, bool invert, float amp, double bitRateScale) {
    std::vector<float> out;
    out.reserve((size_t)(bits.size() * fs / 1200.0) + 8);
    double phase = 0;
    double t = 0;   // fractional sample position of the next bit boundary
    const double spb = fs / (1200.0 * bitRateScale);
    size_t produced = 0;
    for (size_t k = 0; k < bits.size(); k++) {
        t += spb;
        const bool one = bits[k] != 0;
        const double f = (one != invert) ? 1200.0 : 1800.0;
        while ((double)produced < t) {
            out.push_back(amp * (float)std::sin(phase));
            phase += 2 * kPi * f / fs;
            if (phase > 2 * kPi) phase -= 2 * kPi;
            produced++;
        }
    }
    return out;
}

std::vector<std::complex<float>> fmModulate(const std::vector<float>& audio, double fs, double devHz, double offsetHz) {
    std::vector<std::complex<float>> iq(audio.size());
    double ph = 0, car = 0;
    for (size_t i = 0; i < audio.size(); i++) {
        ph += 2 * kPi * devHz * audio[i] / fs;
        car += 2 * kPi * offsetHz / fs;
        ph = std::fmod(ph, 2 * kPi);
        car = std::fmod(car, 2 * kPi);
        const double a = ph + car;
        iq[i] = std::complex<float>((float)std::cos(a), (float)std::sin(a));
    }
    return iq;
}

}  // namespace dect2::mpt1327
