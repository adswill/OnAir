// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DCF77 phase code by correlation: the chip sequence's start and the bit of every second, with noise and a carrier offset (simulated).
#include "dect2/time/dcf77_phase.h"
#include "dect2/common/stable_random.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
int main() {
    { Dcf77Phase d; const auto& p = d.pn(); CHECK(p.size() == 512, "pn length"); const char* w = "00000100011000010011100101010110000"; for (int i = 0; w[i]; i++) CHECK((p[(size_t)i] < 0) == (w[i] == '1'), "pn chip %d", i); }
    const double chip = 120.0 / 77500.0 * 8000.0;
    std::mt19937 rng(4); dect2::normal_distribution<float> nd(0.f, 1.f);
    for (double cnr : {30.0, 15.0, 8.0}) for (double cfo : {0.0, 0.2}) for (int startOff : {0, 3200}) {
        Dcf77Phase d; const auto& pn = d.pn();
        std::vector<int> sent;
        const int fs = 8000;
        double ph = 0;
        for (int s = 0; s < 40; s++) {
            const int bit = (s * 7 + 3) % 2;
            sent.push_back(bit);
            for (int i = 0; i < fs; i++) {
                const int j = (i + fs - startOff) % fs;
                double dev = 0;
                if (j >= 1600) { const int c = (int)std::floor((j - 1600) / chip); if (c < 512) dev = (pn[(size_t)c] > 0 ? 1 : -1) * (bit ? -1 : 1) * 15.6 * M_PI / 180; }
                ph += 2 * M_PI * cfo / fs;
                std::complex<float> z = std::polar<float>(1, (float)(ph + dev));
                const float sg = (float)std::pow(10.0, -cnr / 20) / std::sqrt(2.f);
                z += std::complex<float>(nd(rng), nd(rng)) * sg;
                d.feed(z);
            }
        }
        auto st = d.status(); auto b = d.lastBits();
        int ok = 0, tot = 0;
        for (int i = 20; i < 60; i++) { if (b[(size_t)i] >= 0) { tot++; if (b[(size_t)i] == sent[(size_t)(i - 20)]) ok++; } }
        printf("CNR %.0f, offset %.1f Hz, start %d: locked %d, corr %.2f, offset %d, bits %d / %d right\n", cnr, cfo, startOff, st.locked, st.corr, st.secondOffset, ok, tot);
        if (cnr >= 15) CHECK(st.locked && ok == tot && tot >= 35, "CNR %.0f start %d: %d/%d", cnr, startOff, ok, tot);
    }
    printf(fails ? "dcf phase FAILED\n" : "dcf phase passed\n");
    return fails ? 1 : 0;
}
