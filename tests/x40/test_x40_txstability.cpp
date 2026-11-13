// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Transmitter frequency stability meter: transmissions with a warm-up (an exponential settling from a start offset), a drift, a tone modulation that must average out, carrier offsets and noise; the start
// and final frequency, the settling time, the drift and the mean are compared with the generator; short key-ups are not analysed; noise alone gives nothing. Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Iv { double a, b; };
struct Tx {
    double fs, final0, startOff, tau, drift, modDev, noise; std::vector<Iv> iv; Rng rng{3}; double ph = 0;
    std::vector<cf32> operator()(double t0, size_t n) {
        std::vector<cf32> x(n); const float sg = (float)std::sqrt(noise / 2);
        for (size_t i = 0; i < n; i++) {
            const double t = t0 + i / fs; double amp = 0, f = 0;
            for (auto& v : iv) if (t >= v.a && t < v.b) { const double u = t - v.a; amp = std::min(1.0, u / 0.004) * std::min(1.0, (v.b - t) / 0.004); f = final0 + startOff * std::exp(-u / tau) + drift * u + modDev * std::sin(2 * kPi * 1000 * t); }
            ph += 2 * kPi * f / fs;
            x[i] = (float)amp * cf32((float)std::cos(ph), (float)std::sin(ph)) + cf32(sg * rng.g(), sg * rng.g());
        }
        return x;
    }
};
static double num(const std::string& s, const char* key) { const char* p = strstr(s.c_str(), key); return p ? atof(p + strlen(key)) : -9999; }

int main() {
    const struct { double fs, final0, startOff, tau, drift, mod; } cs[] = {{48000, 500, 250, 0.15, 0, 0}, {48000, -1200, -400, 0.3, 2.0, 1000}, {96000, 100, 120, 0.08, -1.5, 0}, {250000, 2000, 600, 0.5, 0.5, 800}};
    for (auto& c : cs) {
        Tx tx{c.fs, c.final0, c.startOff, c.tau, c.drift, c.mod, noiseFor(35, 16000, c.fs), {{4.0, 10.0}, {14.0, 14.4}, {18.0, 22.0}}};
        auto t = runChunks(x40::TxStability, c.fs, 26.0, std::ref(tx), 0, 146.52e6, 1.0);
        std::vector<std::string> rows; for (auto& r : t.rows) if (r.kind == "Transmission") rows.push_back(r.text);
        CHECK(rows.size() == 2, "fs %.0f: %zu transmissions analysed (the 0.4 s key-up is skipped), want 2", c.fs, rows.size());
        if (rows.size() < 2) continue;
        for (size_t k = 0; k < 2; k++) {
            const double len = k == 0 ? 6.0 : 4.0; const double u1 = len;
            const double wantStart = c.final0 + c.startOff * c.tau / 0.15 * (std::exp(-0.05 / c.tau) - std::exp(-0.2 / c.tau)), wantFinal = c.final0 + c.drift * (u1 - 0.15), settleBand = std::max(10.0, 0.1 * c.startOff);
            const double wantSettle = c.tau * std::log(std::fabs(c.startOff) / settleBand);
            const double st = num(rows[rows.size() - 1 - k], "start "), fin = num(rows[rows.size() - 1 - k], "final "), se = num(rows[rows.size() - 1 - k], "settled in "), dr = num(rows[rows.size() - 1 - k], "drift ");
            if (k == 0) printf("  fs %.0f: start %+.0f (want about %+.0f), final %+.0f (%+.0f), settled in %.2f s (%.2f), drift %+.2f (%+.2f)\n", c.fs, st, wantStart, fin, wantFinal, se, wantSettle, dr, c.drift);
            CHECK(std::fabs(fin - wantFinal) < 25, "final %+.0f want %+.0f", fin, wantFinal);
            CHECK(std::fabs(st - wantStart) < 0.12 * std::fabs(c.startOff) + 25, "start %+.0f want about %+.0f", st, wantStart);
            CHECK(std::fabs(se - wantSettle) < 0.12 + 0.2 * wantSettle, "settle %.2f want %.2f", se, wantSettle);
            CHECK(std::fabs(dr - c.drift) < (k == 0 ? 1.5 : 5.0), "drift %+.2f want %+.2f", dr, c.drift);
        }
    }
    { ToneScene sc(48000, 1.0, 3); auto t = runChunks(x40::TxStability, 48000, 60.0, std::ref(sc), 0, 146.52e6, 1.0); CHECK(t.rows.empty(), "noise: %zu rows", t.rows.size()); }
    printf(fails ? "x40 txstability: %d FAILED\n" : "x40 txstability: ok\n", fails);
    return fails ? 1 : 0;
}
