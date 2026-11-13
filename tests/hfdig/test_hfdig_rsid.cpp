// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// RSID: the GF(16) code against the table rows of fldigi's rsid.cxx, then generated bursts at several audio frequencies, noise, and a real-looking audio rate. Simulated audio only.
#include "dect2/hfdig/hfdig_rsid.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"
#undef CHECK
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static std::vector<float> burst(int code, double center, double noise, unsigned seed) {
    const double fs = 8000, sp = 11025.0 / 1024.0, base = center - 7 * sp;
    const double symSec = 1024.0 / 11025.0;
    uint8_t s[15];
    rsidEncode(code, s);
    std::mt19937 g(seed);
    std::normal_distribution<float> nd(0, (float)noise);
    std::vector<float> out;
    double ph = 0, t = 0;
    const double start = 0.3, dur = 15 * symSec, end = start + dur + 0.4;
    while (t < end) {
        float v = 0;
        if (t >= start && t < start + dur) {
            const int k = (int)((t - start) / symSec);
            ph += 2 * M_PI * (base + s[k] * sp) / fs;
            v = 0.4f * (float)std::sin(ph);
        }
        out.push_back(v + (noise > 0 ? nd(g) : 0.f));
        t += 1 / fs;
    }
    return out;
}

int main() {
    // code 1 (BPSK31) by hand from the Squares-table recurrence
    uint8_t s[15];
    rsidEncode(1, s);
    int nonzero = 0; for (int i = 0; i < 15; i++) nonzero += s[i] != 0;
    CHECK(nonzero >= 10);
    // any two listed codes differ in at least 13 of 15 symbols: the code is RS(15,3)
    const auto& modes = rsidModes();
    int minDist = 15;
    for (size_t a = 0; a < modes.size(); a++) for (size_t b = a + 1; b < modes.size(); b++) {
        uint8_t x[15], y[15]; rsidEncode(modes[a].code, x); rsidEncode(modes[b].code, y);
        int d = 0; for (int i = 0; i < 15; i++) d += x[i] != y[i];
        if (d < minDist) minDist = d;
    }
    printf("min distance %d\n", minDist);
    CHECK(minDist >= 13);
    for (int code : {1, 57, 71, 138}) for (double fc : {800.0, 1500.0, 2240.3}) for (double noise : {0.0, 0.25}) {
        RsidDetector d;
        const auto a = burst(code, fc, noise, (unsigned)code + 2);
        d.feedAudio(a.data(), a.size());
        RsidHit h; bool got = d.poll(h);
        if (!got || h.code != code || std::fabs(h.audioHz - fc) > 8.0) printf("  code %d fc %.1f noise %.2f: %s %d %.1f\n", code, fc, noise, got ? "got" : "miss", got ? h.code : 0, got ? h.audioHz : 0);
        CHECK(got && h.code == code && std::fabs(h.audioHz - fc) <= 8.0);
        RsidHit h2; CHECK(!d.poll(h2));
    }
    // noise only: no false hit in a long run
    { RsidDetector d; std::mt19937 g(5); std::normal_distribution<float> nd(0, 0.3f); std::vector<float> n(8000 * 20); for (auto& v : n) v = nd(g); d.feedAudio(n.data(), n.size()); RsidHit h; CHECK(!d.poll(h)); }
    return testSummary("hfdig_rsid");
}
