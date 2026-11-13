// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TD-SCDMA (1.28 Mcps TDD) codes, test transmitter and cell search (tdscdma.h for the references).
#include "dect2/tdscdma/tdscdma.h"
#include "dect2/common/stable_random.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/dsp/rrc.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstdio>
#include <random>

namespace dect2::tdscdma {

namespace {
#include "tdscdma_tables.inc"

std::vector<int> hexBits(const char* h, int n) {   // MSB first, 1 -> +1, 0 -> -1
    std::vector<int> v;
    for (const char* p = h; *p && (int)v.size() < n; p++) {
        const int d = *p <= '9' ? *p - '0' : *p - 'A' + 10;
        for (int b = 3; b >= 0; b--) v.push_back((d >> b) & 1 ? 1 : -1);
    }
    return v;
}
cf32 jpow(int e) { e = ((e % 4) + 4) % 4; return e == 0 ? cf32(1, 0) : e == 1 ? cf32(0, 1) : e == 2 ? cf32(-1, 0) : cf32(0, -1); }
using cd = std::complex<double>;
constexpr double kPi = 3.14159265358979323846;

std::vector<float> rrcTaps(int H) {
    std::vector<float> h(2 * H + 1);
    double e = 0;
    for (int k = -H; k <= H; k++) { h[(size_t)(k + H)] = (float)dsp::rrc(k / 2.0, 0.22); e += h[(size_t)(k + H)] * h[(size_t)(k + H)]; }
    for (auto& v : h) v = (float)(v / std::sqrt(e / 2));
    return h;
}
}  // namespace

std::vector<cf32> syncDl(int code) {
    const auto b = hexBits(kSyncDlHex[code & 31], 64);
    std::vector<cf32> s(64);
    for (int i = 0; i < 64; i++) s[(size_t)i] = jpow(-(i + 1)) * (float)b[(size_t)i];   // (-j)^i, i from 1
    return s;
}
std::vector<cf32> basicMidamble(int code) {
    const auto b = hexBits(kMidHex[code & 127], 128);
    std::vector<cf32> m(128);
    for (int i = 0; i < 128; i++) m[(size_t)i] = jpow(i + 1) * (float)b[(size_t)i];      // j^i, i from 1
    return m;
}
std::vector<cf32> midamble(int code, int k, int K) {
    const auto m = basicMidamble(code);
    const int W = 128 / K;
    std::vector<cf32> o(144);
    for (int i = 0; i < 144; i++) o[(size_t)i] = m[(size_t)((i + (K - k) * W) % 128)];   // m(k)_i = m_(i + (K - k) W) of the periodic extension
    return o;
}
const signed char* scramblingCode(int code) { return kScr[code & 127]; }

// ---------------------------------------------------------------- the test transmitter
std::vector<cf32> generate(const std::vector<TxCell>& cells, size_t samples, double snrDb, unsigned seed) {
    std::vector<cf32> sum(samples, cf32(0, 0));
    std::mt19937 rng(seed);
    const int H = 12;
    const auto h = rrcTaps(H);
    const size_t nChips = samples / 2 + 2 * H;
    for (const TxCell& c : cells) {
        std::vector<cf32> ch(nChips, cf32(0, 0));
        const int g = c.cellParam / 4;
        const auto sd = syncDl(g);
        auto qp = [&](int bits) { return cf32(bits & 1 ? -1.f : 1.f, bits & 2 ? -1.f : 1.f) * 0.70710678f; };
        const int64_t first = c.chipOffset <= 0 ? (int64_t)(-c.chipOffset) / kSubframeChips : -(((int64_t)c.chipOffset + kSubframeChips - 1) / kSubframeChips);
        for (int64_t s = first; ; s++) {
            const int64_t base = (int64_t)c.chipOffset + s * kSubframeChips;
            if (base >= (int64_t)nChips) break;
            const int64_t abs = 2LL * c.sfn0 + s;                 // absolute sub-frame count (2 per frame)
            const int64_t sfn = abs >= 0 ? abs / 2 : (abs - 1) / 2;
            const int param = (sfn & 1) ? oddParam(c.cellParam) : c.cellParam;
            const int quad = (int)(((abs % 4) + 4) % 4);
            const int64_t qn = abs >= 0 ? abs / 4 : (abs - 3) / 4;
            const int ph = c.s2Every && (qn & 1) ? kS2[quad] : kS1[quad];
            auto put = [&](int64_t chip, cf32 v) { const int64_t k = base + chip; if (k >= 0 && k < (int64_t)nChips) ch[(size_t)k] += v; };
            // time slot 0: the two P-CCPCH codes (spreading factor 16, cell scrambling code) with random data, the midambles m(1) + m(2)
            const signed char* sc = scramblingCode(param);
            for (int f = 0; f < 2; f++) {
                const int start = f ? 496 : 0;
                for (int sym = 0; sym < 22; sym++) {
                    const cf32 d1 = qp((int)(rng() & 3)), d2 = qp((int)(rng() & 3));
                    for (int q = 0; q < 16; q++) {
                        const int i = sym * 16 + q;
                        // OVSF codes c16^(1) = all ones and c16^(2) = (+1 x 8, -1 x 8): the first two of the code tree
                        const float o2 = q < 8 ? 1.f : -1.f;
                        const cf32 v = (d1 + d2 * o2) * (float)sc[q] * jpow(-((i % 16) + 1)) * 0.5f;
                        put(start + i, v);
                    }
                }
            }
            const auto m1 = midamble(param, 1), m2 = midamble(param, 2);
            for (int i = 0; i < 144; i++) put(kMidChip + i, (m1[(size_t)i] + m2[(size_t)i]) * 0.70710678f);
            // DwPTS: 32 chips guard, the SYNC-DL turned by the quadruple's phase relative to m(1)
            const cf32 rot = std::polar(1.f, (float)(ph * kPi / 180));
            for (int i = 0; i < 64; i++) put(kSyncDlChip + i, sd[(size_t)i] * rot);
            // time slots 4 - 6: downlink traffic (random symbols, spreading factor 16 with 8 codes), with the cell's midamble m(3)
            const auto m3 = midamble(param, 3);
            const int64_t ts4 = 864 + 96 + 96 + 160 + 3 * 864;
            for (int t = 0; t < 3; t++) {
                const int64_t ts = ts4 + t * 864;
                for (int i = 0; i < 704; i++) {
                    const int64_t chip = ts + (i < 352 ? i : i + 144);
                    put(chip, cf32((rng() & 1) ? -0.5f : 0.5f, (rng() & 1) ? -0.5f : 0.5f) * 1.41421356f);
                }
                for (int i = 0; i < 144; i++) put(ts + 352 + i, m3[(size_t)i]);
            }
        }
        const double w = 2 * kPi * c.cfoHz / kFs;
        for (size_t n = 0; n < samples; n++) {
            cf32 a(0, 0);
            for (int k = -H; k <= H; k++) {
                const int64_t m = (int64_t)n - k;
                if (m < 0 || (m & 1)) continue;
                const size_t ci = (size_t)(m / 2);
                if (ci < ch.size()) a += ch[ci] * h[(size_t)(k + H)];
            }
            sum[n] += a * std::polar((float)c.amp, (float)(w * (double)n));
        }
    }
    double p = 0;
    for (auto& v : sum) p += std::norm(v);
    p /= (double)std::max<size_t>(1, samples);
    if (snrDb < 200) {
        dect2::normal_distribution<float> gd(0.f, (float)std::sqrt(p * std::pow(10.0, -snrDb / 10) / 2));
        for (auto& v : sum) v += cf32(gd(rng), gd(rng));
    }
    return sum;
}

// ---------------------------------------------------------------- the search
std::vector<Found> search(const cf32* x, size_t n, const SearchOpts& o, double* ms) {
    const auto tc = std::chrono::steady_clock::now();
    std::vector<Found> out;
    const int SF = 2 * kSubframeChips;   // samples per sub-frame
    if (n < (size_t)(4 * SF)) return out;
    const int H = 12;
    const auto h = rrcTaps(H);
    double he = 0; for (float v : h) he += v * v;
    std::vector<cf32> y(n);
    for (size_t i = 0; i < n; i++) {
        cf32 a(0, 0);
        for (int k = -H; k <= H; k++) { const int64_t j = (int64_t)i + k; if (j >= 0 && j < (int64_t)n) a += x[j] * h[(size_t)(k + H)]; }
        y[i] = a * (float)(1 / he);
    }
    // ---- SYNC-DL: FFT correlation with each code, folded over the sub-frames
    int N = 1; while (N < (int)n + 256) N <<= 1;
    Fft F(N);
    std::vector<cf32> Y(N, cf32(0, 0));
    std::copy(y.begin(), y.end(), Y.begin());
    F.forward(Y.data());
    const int nSub = (int)((n - 256) / (size_t)SF);
    std::vector<std::vector<float>> fold(32, std::vector<float>((size_t)SF, 0.f));
    std::vector<cf32> R(N);
    for (int c = 0; c < 32; c++) {
        std::fill(R.begin(), R.end(), cf32(0, 0));
        const auto s = syncDl(c);
        for (int i = 0; i < 64; i++) R[(size_t)(2 * i)] = s[(size_t)i];
        F.forward(R.data());
        for (int k = 0; k < N; k++) R[(size_t)k] = Y[(size_t)k] * std::conj(R[(size_t)k]);
        F.inverse(R.data());
        for (int t = 0; t < nSub * SF; t++) fold[(size_t)c][(size_t)(t % SF)] += std::norm(R[(size_t)t]);
    }
    std::vector<float> all;
    all.reserve((size_t)32 * SF / 8);
    for (int c = 0; c < 32; c++) for (int t = 0; t < SF; t += 8) all.push_back(fold[(size_t)c][(size_t)t]);
    std::nth_element(all.begin(), all.begin() + (ptrdiff_t)(all.size() / 2), all.end());
    const double med = std::max(1e-30f, all[all.size() / 2]);
    struct Cand { int code, t; double v; };
    std::vector<Cand> cands;
    for (int c = 0; c < 32; c++) {
        int bt = 0; for (int t = 1; t < SF; t++) if (fold[(size_t)c][(size_t)t] > fold[(size_t)c][(size_t)bt]) bt = t;
        const double v = fold[(size_t)c][(size_t)bt];
        if (v > o.thresh * med) cands.push_back({c, bt, v});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.v > b.v; });
    if (getenv("ONAIR_TDS_DEBUG")) for (auto& c : cands) fprintf(stderr, "[tds] code %d at %d, %.1f x median\n", c.code, c.t, c.v / med);
    for (const Cand& cd0 : cands) {
        if ((int)out.size() >= o.maxCells) break;
        Found f;
        f.syncDl = cd0.code;
        const auto s = syncDl(cd0.code);
        // the sub-frame holding the first whole sub-frame
        int t0 = cd0.t - 2 * kSyncDlChip;
        while (t0 < 0) t0 += SF;
        f.subframeStart = t0;
        const int subs = (int)((n - (size_t)t0 - 16) / (size_t)SF);
        if (subs < 2) continue;
        f.subframes = subs;
        auto sdAt = [&](int t, int i0, int i1, double w) { cd a = 0; for (int i = i0; i < i1; i++) { const double tt = t + 2.0 * i; a += cd(y[(size_t)(t + 2 * i)] * std::conj(s[(size_t)i])) * std::polar(1.0, -w * tt); } return a; };
        // carrier from the two halves of every SYNC-DL
        cd d = 0;
        for (int k = 0; k < subs; k++) { const int t = t0 + k * SF + 2 * kSyncDlChip; d += sdAt(t, 32, 64, 0) * std::conj(sdAt(t, 0, 32, 0)); }
        const double w = std::arg(d) / 64.0;   // rad per sample (32 chips = 64 samples apart)
        f.cfoHz = w * kFs / (2 * kPi);
        // ---- per sub-frame: midamble candidates of the code group, SYNC-DL phase against m(1)
        std::vector<cd> sdv((size_t)subs);
        std::vector<std::array<double, 4>> score((size_t)subs);
        std::vector<std::array<cd, 4>> h1((size_t)subs);
        std::vector<std::vector<cf32>> base(4), ref1(4);
        for (int p = 0; p < 4; p++) {
            base[(size_t)p] = basicMidamble(4 * cd0.code + p);
            const auto m1 = midamble(4 * cd0.code + p, 1);
            ref1[(size_t)p].assign(m1.begin() + 16, m1.end());
        }
        double pw = 0, pn = 0;
        for (int k = 0; k < subs; k++) {
            const int sb = t0 + k * SF;
            sdv[(size_t)k] = sdAt(sb + 2 * kSyncDlChip, 0, 64, w) / 64.0;
            cf32 win[128];
            for (int i = 0; i < 128; i++) { const double tt = sb + 2.0 * (kMidChip + 16 + i); win[i] = y[(size_t)(sb + 2 * (kMidChip + 16 + i))] * std::polar(1.f, (float)(-w * tt)); }
            for (int p = 0; p < 4; p++) {
                double best = 0;
                const auto& b = base[(size_t)p];
                for (int sh = 0; sh < 128; sh += 1) {
                    cd a = 0;
                    for (int i = 0; i < 128; i++) a += cd(win[i] * std::conj(b[(size_t)((i + sh) & 127)]));
                    best = std::max(best, std::norm(a));
                }
                score[(size_t)k][(size_t)p] = best;
                cd a = 0;
                for (int i = 0; i < 128; i++) a += cd(win[i] * std::conj(ref1[(size_t)p][(size_t)i]));
                h1[(size_t)k][(size_t)p] = a / 128.0;
            }
            pw += std::norm(sdv[(size_t)k]);
            for (int i = 0; i < 32; i++) pn += std::norm(y[(size_t)(sb + 2 * (864 + i))]);   // the DwPTS guard
        }
        f.levelDbfs = 10 * std::log10(std::max(1e-20, pw / subs));
        f.snrDb = 10 * std::log10(std::max(1e-6, (pw / subs) / std::max(1e-20, pn / (32.0 * subs))));
        // ---- the parameter pair and the frame alignment: frames of 2 sub-frames alternate between p and p ^ 1
        double bestH = -1, second = -1; int bp = 0, ba = 0;
        for (int p = 0; p < 4; p++)
            for (int a = 0; a < 2; a++) {
                double sum = 0;
                for (int k = 0; k < subs; k++) { const int fr = (k + a) / 2; const int par = (fr & 1) ? (p ^ 1) : p; sum += score[(size_t)k][(size_t)par]; }
                if (sum > bestH) { second = bestH; bestH = sum; bp = p; ba = a; } else if (sum > second) second = sum;
            }
        // the competitor that matters: another pair (p ^ 2) with any alignment
        double other = 0;
        for (int a = 0; a < 2; a++) { double sum = 0; for (int k = 0; k < subs; k++) { const int fr = (k + a) / 2; const int par = (fr & 1) ? (bp ^ 3) : (bp ^ 2); sum += score[(size_t)k][(size_t)par]; } other = std::max(other, sum); }
        f.midScore = other > 0 ? (float)(bestH / other) : 0.f;
        // ---- the SYNC-DL phase quadruples: where they start is an even SFN, sub-frame 0
        // The phase of each sub-frame against m(1); a carrier offset error turns them all by the same angle (the SYNC-DL comes about
        // 0.4 ms after the midamble), so that common angle is searched too, the smallest one that explains the most quadruples.
        std::vector<double> phs((size_t)subs);
        for (int k = 0; k < subs; k++) {
            const int fr = (k + ba) / 2;
            const int par = (fr & 1) ? (bp ^ 1) : bp;
            phs[(size_t)k] = std::arg(sdv[(size_t)k] * std::conj(h1[(size_t)k][(size_t)par])) * 180 / kPi;
        }
        int bestOff = -1, bestCnt = 0, s1 = 0, s2 = 0, bestDelta = 999;
        for (int delta = -175; delta <= 180; delta += 5)
            for (int off = 0; off < 4; off++) {
                int cnt = 0, c1 = 0, c2 = 0;
                for (int k0 = (4 - off) % 4; k0 + 4 <= subs; k0 += 4) {
                    int m1 = 0, m2 = 0;
                    for (int j = 0; j < 4; j++) {
                        const double ph = phs[(size_t)(k0 + j)] - delta;
                        auto nearV = [&](int deg) { double dd = std::fabs(std::remainder(ph - deg, 360.0)); return dd < 40; };
                        m1 += nearV(kS1[j]); m2 += nearV(kS2[j]);
                    }
                    // S1 and S2 differ in every position: 3 of 4 phases are enough to tell them apart
                    if (m1 >= 3 && m2 <= 1) { cnt++; c1++; } else if (m2 >= 3 && m1 <= 1) { cnt++; c2++; }
                }
                if (cnt > bestCnt || (cnt == bestCnt && cnt > 0 && std::abs(delta) < std::abs(bestDelta))) { bestCnt = cnt; bestOff = off; s1 = c1; s2 = c2; bestDelta = delta; }
            }
        f.quadS1 = s1; f.quadS2 = s2;
        // the common angle is the carrier error times the time from the midamble window's centre to the SYNC-DL's centre
        if (bestCnt > 0) f.cfoHz += bestDelta / 360.0 / (((kSyncDlChip + 32) - (kMidChip + 16 + 64)) / kChipRate);
        const int pairLo = 4 * cd0.code + (bp & 2);
        if (bestOff >= 0 && f.midScore > 1.3f) {
            const int k0 = (4 - bestOff) % 4;                 // a sub-frame 0 of an even SFN frame
            const int fr = (k0 + ba) / 2;
            const int par = (fr & 1) ? (bp ^ 1) : bp;
            f.cellParam = 4 * cd0.code + par;
            // the first whole frame of the block: sub-frames (ba ? 1 : 0) ... its parity relative to the even frame at k0
            const int firstFrameSub = ba ? 1 : 0;
            f.evenFirst = (((k0 - firstFrameSub) / 2) % 2 == 0) ? 1 : 0;
        } else if (f.midScore > 1.3f) f.cellParam = pairLo;   // the pair is known, not which one is the initial
        // a code that only correlates with another cell's signal: no midamble of its group and no SYNC-DL above the guard
        if (!((f.midScore >= 1.5f && f.snrDb > -3) || f.snrDb > 3)) continue;
        out.push_back(f);
        // take this cell's SYNC-DL out, so that a weaker cell's SYNC-DL (cross-correlation up to -10 dB) is measured cleanly
        for (int k = 0; k < subs; k++) {
            const int t = t0 + k * SF + 2 * kSyncDlChip;
            for (int i = 0; i < 64; i++) {
                const double tt = t + 2.0 * i;
                y[(size_t)(t + 2 * i)] -= cf32(sdv[(size_t)k] * cd(s[(size_t)i]) * std::polar(1.0, w * tt));
            }
        }
    }
    if (ms) *ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tc).count();
    return out;
}

}  // namespace dect2::tdscdma
