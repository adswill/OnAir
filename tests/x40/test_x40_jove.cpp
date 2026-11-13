// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Radio Jove burst finder: short and long bursts of different strength, a slow baseline drift, and the false-burst rate on noise alone.
#include "x40_gen.h"
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    const double fs = 96000;
    for (double extra : {3.0, 1.5, 0.8}) {   // 0.8 (2.6 dB) is below what the finder can hold together: shown, not required
        auto t = run(x40::Jove, jove(fs, 60.0, {{10, 0.06, extra}, {20, 0.5, extra}, {35, 3.0, extra}, {50, 0.08, extra}}), fs);
        const int s = countKind(t, "Short burst"), l = countKind(t, "Long burst");
        printf("burst %.1f dB over the noise: %d short, %d long\n", 10 * std::log10(1 + extra), s, l);
        if (extra > 1) CHECK(s == 2 && l == 2, "extra %.1f: %d short %d long", extra, s, l);
    }
    // a baseline that drifts by 3 dB over the run must not make bursts
    {
        auto x = jove(fs, 120.0, {{60, 0.3, 3.0}});
        for (size_t i = 0; i < x.size(); i++) x[i] *= (float)(1 + 0.4 * i / (double)x.size());
        auto t = run(x40::Jove, x, fs);
        CHECK(countKind(t, "Short burst") + countKind(t, "Long burst") == 1, "drift: %zu rows", t.rows.size());
    }
    // a carrier offset does not matter for a power detector, a signal at the band edge must still be counted
    {
        auto x = jove(fs, 30.0, {{12, 0.4, 3.0}}); mix(x, 1500, 0, fs);
        CHECK(countKind(run(x40::Jove, x, fs), "Long burst") == 1, "off-centre");
    }
    // false bursts on noise alone: 30 minutes of it
    {
        const auto t = run(x40::Jove, jove(fs, 1800.0, {}, 77), fs);
        printf("noise only, 30 min: %zu false bursts\n", t.rows.size());
        CHECK(t.rows.size() <= 2, "false bursts %zu", t.rows.size());
    }
    printf(fails ? "x40 jove: %d FAILED\n" : "x40 jove: ok\n", fails);
    return fails ? 1 : 0;
}
