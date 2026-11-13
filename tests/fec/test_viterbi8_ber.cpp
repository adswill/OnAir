// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The 8-bit two-run Viterbi (fecp::viterbiRange8x2, DVB-T) against the exact 16-bit one (fecp::viterbiRange): bit error rate on the same
// noisy K = 7 code words, rate 1/2 and DVB-T's 2/3 puncturing, BPSK in AWGN near the waterfall, each decoder fed the way DVB-T feeds it
// (exact: soft values scaled to a mean of 22 within +-127; 8-bit: a mean of 14 within +-31), in DVB-T's chunks (16384 steps, 192 overlap).
// The 8-bit decoder may make at most 4% more errors than the exact one plus three standard deviations of the count (4% of the BER is
// about 0.015 dB at these slopes; the limit for a change is 0.05 dB). Where the 8-bit decoder does not exist (no NEON) it is the exact one.
#include "dect2/fec/fec_proto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace dect2;

namespace {
int fails = 0;

void quant(const std::vector<float>& llr, double target, int lim, std::vector<int8_t>& out) {
    double mean = 0; size_t c = 0;
    for (float v : llr) if (v != 0) { mean += std::fabs(v); c++; }
    mean /= (double)c;
    const float s = (float)(target / mean);
    out.resize(llr.size());
    for (size_t i = 0; i < llr.size(); i++) { const float v = std::max(-(float)lim, std::min((float)lim, llr[i] * s)); out[i] = (int8_t)std::lround(v); }
}

void run(int rate, double ebn, size_t nInfo, std::mt19937& rng) {
    std::normal_distribution<double> nd(0, 1);
    const double R = rate ? 2.0 / 3 : 0.5, sig = std::sqrt(1.0 / (2 * R * std::pow(10.0, ebn / 10)));
    std::vector<uint8_t> info(nInfo);
    for (auto& b : info) b = rng() & 1;
    std::vector<float> llr(2 * nInfo);
    unsigned reg = 0;
    for (size_t t = 0; t < nInfo; t++) {
        reg = ((unsigned)info[t] << 6) | (reg >> 1);   // the decoder's trellis: the new bit enters at the top
        const int x = __builtin_parity(reg & 0x79), y = __builtin_parity(reg & 0x5B);
        for (int k = 0; k < 2; k++) {
            const bool punct = rate == 1 && (t % 2 == 1) && k == 1;   // 2/3: X1 Y1 X2
            const double v = ((k ? y : x) ? -1 : 1) + sig * nd(rng);
            llr[2 * t + k] = punct ? 0.f : (float)(2 * v / (sig * sig));
        }
    }
    std::vector<int8_t> s16, s8;
    quant(llr, 22, 127, s16);
    quant(llr, 14, fecp::kViterbi8Max, s8);
    const size_t C = 16384, L = 192;
    long e16 = 0, e8 = 0, n = 0;
    std::vector<uint8_t> b1, b2, b3;
    for (size_t c0 = 0; c0 + 2 * C <= nInfo; c0 += 2 * C) {
        size_t lo[2], hi[2];
        for (int k = 0; k < 2; k++) { const size_t a = c0 + k * C; lo[k] = a > L ? a - L : 0; hi[k] = std::min(nInfo, a + C + L); }
        long m;
        for (int k = 0; k < 2; k++) {
            fecp::viterbiRange(s16.data() + 2 * lo[k], hi[k] - lo[k], b1, &m);
            for (size_t i = c0 + k * C; i < c0 + (k + 1) * C; i++) e16 += b1[i - lo[k]] != info[i];
        }
        if (fecp::viterbi8x2Available()) fecp::viterbiRange8x2(s8.data() + 2 * lo[0], hi[0] - lo[0], s8.data() + 2 * lo[1], hi[1] - lo[1], b2, b3, &m, &m);
        else { fecp::viterbiRange(s16.data() + 2 * lo[0], hi[0] - lo[0], b2, &m); fecp::viterbiRange(s16.data() + 2 * lo[1], hi[1] - lo[1], b3, &m); }
        for (size_t i = c0; i < c0 + C; i++) e8 += b2[i - lo[0]] != info[i];
        for (size_t i = c0 + C; i < c0 + 2 * C; i++) e8 += b3[i - lo[1]] != info[i];
        n += 2 * (long)C;
    }
    const double limit = 1.04 * (double)e16 + 3 * std::sqrt((double)e16) + 3;
    const bool ok = (double)e8 <= limit;
    printf("rate %s Eb/N0 %.1f dB: bit errors exact %ld, 8-bit %ld of %ld (BER %.2e / %.2e)%s\n", rate ? "2/3" : "1/2", ebn, e16, e8, n, (double)e16 / n, (double)e8 / n,
           ok ? "" : "  FAIL");
    if (!ok) fails++;
}
}   // namespace

int main() {
    std::mt19937 rng(17);
    run(0, 2.0, 2000000, rng);
    run(0, 2.5, 2000000, rng);
    run(1, 3.0, 2000000, rng);
    run(1, 3.5, 2000000, rng);
    run(1, 4.0, 3000000, rng);
    printf(fails ? "viterbi8 ber: %d FAILED\n" : "viterbi8 ber: ok\n", fails);
    return fails ? 1 : 0;
}
