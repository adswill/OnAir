// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AM frequency diversity: one programme on two frequencies with a delay between them and fades that come at different times. The output must beat either channel alone;
// the delay must be found. FadeTracker must find a regular fade and its period.
#include "dect2/general/am_diversity.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    {   // fade predictor: a sine fade of 8 s period, sampled 4 times a second
        FadeTracker t; t.configure(4);
        std::mt19937 r(2); std::normal_distribution<float> g(0, 0.5f);
        for (int i = 0; i < 200; i++) t.push(-40 + 8 * (float)std::sin(2 * M_PI * i / 32.0) + g(r));
        printf("fade: depth %.1f dB, period %.1f s, next dip in %.1f s\n", t.depthDb(), t.periodSec(), t.nextFadeIn());
        CHECK(t.depthDb() > 8, "depth %.1f", t.depthDb());
        CHECK(std::fabs(t.periodSec() - 8) < 1, "period %.1f", t.periodSec());
        CHECK(t.nextFadeIn() >= 0 && t.nextFadeIn() <= 8, "next %.1f", t.nextFadeIn());
    }
    for (int lagMs : {0, 37, 180, -90}) {
        const int rate = 48000; const int total = 40 * rate;
        const int lag = lagMs * rate / 1000;
        std::mt19937 r(3); std::normal_distribution<float> g(0, 1);
        // the programme: narrow-band noise (speech stand-in)
        std::vector<float> prog((size_t)total + 96000);
        float s1 = 0, s2 = 0;
        for (auto& v : prog) { s1 += (g(r) - s1) * 0.15f; s2 += (s1 - s2) * 0.3f; v = s2 * 4; }
        AmDiversity d; d.configure(rate);
        std::vector<float> out, a, b, wa, wb;
        double sSingle[2] = {0, 0}, nSingle[2] = {0, 0};
        const float noiseAmp = 0.25f;
        const int hop = 4800;
        for (int i = 0; i + hop <= total; i += hop) {
            a.resize((size_t)hop); b.resize((size_t)hop);
            const double t = i / (double)rate;
            const float ga = 0.2f + 0.8f * (float)std::pow(std::cos(M_PI * t / 11.0), 2.0), gb = 0.2f + 0.8f * (float)std::pow(std::sin(M_PI * t / 11.0), 2.0);
            for (int k = 0; k < hop; k++) {
                const float src = prog[(size_t)(i + k + 48000)];
                const int ib = i + k + 48000 - lag;
                const float srcB = prog[(size_t)ib];
                a[(size_t)k] = src * ga + noiseAmp * g(r); b[(size_t)k] = srcB * gb + noiseAmp * g(r);
                sSingle[0] += src * ga * src * ga; nSingle[0] += noiseAmp * noiseAmp; sSingle[1] += srcB * gb * srcB * gb; nSingle[1] += noiseAmp * noiseAmp;
            }
            wa.insert(wa.end(), a.begin(), a.end()); wb.insert(wb.end(), b.begin(), b.end());
            d.process(a.data(), b.data(), (size_t)hop, 20 * std::log10(ga / noiseAmp), 20 * std::log10(gb / noiseAmp), out);
        }
        // every path against the programme by the same measure: the correlation over the part after the start
        const auto score = [&](const std::vector<float>& y, int offset) {
            double best = -100;
            for (int L = offset - 800; L <= offset + 800; L += 8) {
                double xy = 0, xx = 0, yy = 0;
                for (size_t i = (size_t)(6 * rate); i + 1 < y.size() && i < (size_t)(36 * rate); i += 3) { const double x = prog[i + (size_t)L], v = y[i]; xy += x * v; xx += x * x; yy += v * v; }
                const double c = xy / std::sqrt(std::max(xx * yy, 1e-20)), snr = 10 * std::log10(std::max(c * c, 1e-6) / std::max(1 - c * c, 1e-6));
                best = std::max(best, snr);
            }
            return best;
        };
        const double bestSnr = score(out, 48000 + std::max(0, -lag)), singleA = score(wa, 48000), singleB = score(wb, 48000 - lag);
        const int bestLag = 48000;
        printf("lag %+4d ms: delay found %+6d samples (expected %+6d), switches %d, output SNR %.1f dB, single channels %.1f / %.1f dB (best offset %d)\n", lagMs, d.delaySamples(), lag, d.switches(), bestSnr, singleA, singleB, bestLag);
        CHECK(std::abs(d.delaySamples() - lag) <= 24, "lag %d: delay %d", lag, d.delaySamples());
        CHECK(d.switches() >= 2, "lag %d: only %d switches", lag, d.switches());
        CHECK(bestSnr > std::max(singleA, singleB) + 1.5, "lag %d: output %.1f dB against %.1f / %.1f", lag, bestSnr, singleA, singleB);
    }
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
