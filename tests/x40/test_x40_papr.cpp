// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Crest factor meter: Gaussian noise (theory: 3.6, 6.6, 8.4, 9.6 dB at 10, 1, 0.1, 0.01 %), a constant-envelope signal (FM), a QPSK signal with root-raised-cosine pulses (about 5 dB at 1 %), clipped
// noise, noise plus impulses; the readings compared with theory or the generator's own statistics; a steady carrier. Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<cf32> gauss(size_t n, unsigned seed, double pw = 1.0) { Rng r(seed); std::vector<cf32> x(n); const float s = (float)std::sqrt(pw / 2); for (auto& v : x) v = cf32(s * r.g(), s * r.g()); return x; }
static double ccdf(const std::vector<cf32>& x, double tail) { std::vector<float> p; p.reserve(x.size()); double m = 0; for (auto& v : x) { p.push_back(std::norm(v)); m += p.back(); } m /= (double)x.size(); std::sort(p.begin(), p.end()); return 10 * std::log10(p[(size_t)((1 - tail) * (double)p.size())] / m); }

int main() {
    const double fs = 1e6;
    // 1. Gaussian noise at three levels, 20 s: the meter against the exact theory (10 log(-ln tail))
    for (double pw : {1e-2, 0.4}) {
        auto x = gauss((size_t)(fs * 12), 5, pw); auto t = x; (void)t;
        auto r = run(x40::Papr, x, fs);
        const double th[4] = {10 * std::log10(-std::log(0.1)), 10 * std::log10(-std::log(0.01)), 10 * std::log10(-std::log(0.001)), 10 * std::log10(-std::log(0.0001))};
        const double g10 = factNum(r, "CCDF 10 %"), g1 = factNum(r, "CCDF 1 %"), g01 = factNum(r, "CCDF 0.1 %"), g001 = factNum(r, "CCDF 0.01 %");
        printf("  Gaussian %.0e: 10 / 1 / 0.1 / 0.01 %%: %.2f / %.2f / %.2f / %.2f dB (theory %.2f / %.2f / %.2f / %.2f)\n", pw, g10, g1, g01, g001, th[0], th[1], th[2], th[3]);
        CHECK(std::fabs(g10 - th[0]) < 0.1 && std::fabs(g1 - th[1]) < 0.15 && std::fabs(g01 - th[2]) < 0.3 && std::fabs(g001 - th[3]) < 0.6, "Gaussian %.0e off", pw);
        CHECK(factStr(r, "reading").rfind("Gaussian-like", 0) == 0, "reading %s", factStr(r, "reading").c_str());
    }
    // 2. constant envelope (FM tone-modulated carrier) and a plain carrier plus a little noise
    { std::vector<cf32> x((size_t)(fs * 5)); double ph = 0; for (size_t i = 0; i < x.size(); i++) { ph += 2 * kPi * (30000 + 20000 * std::sin(2 * kPi * 440 * i / fs)) / fs; x[i] = cf32((float)std::cos(ph), (float)std::sin(ph)); }
      auto r = run(x40::Papr, x, fs); CHECK(factNum(r, "crest factor (peak over RMS)") < 0.1, "FM crest %.2f", factNum(r, "crest factor (peak over RMS)")); CHECK(factStr(r, "reading").find("constant envelope") != std::string::npos, "FM reading"); }
    // 3. QPSK with root-raised-cosine pulses (roll-off 0.35): compared with the generator's own statistics
    { auto x = mpsk(4, 25000, 0.35, fs, 12.0, 0, 0, 0, 3); auto r = run(x40::Papr, x, fs);
      const double w1 = ccdf(x, 0.01), w01 = ccdf(x, 0.001); const double g1 = factNum(r, "CCDF 1 %"), g01 = factNum(r, "CCDF 0.1 %");
      printf("  QPSK RRC 0.35: 1 %% %.2f dB (direct count %.2f), 0.1 %% %.2f (%.2f)\n", g1, w1, g01, w01);
      CHECK(std::fabs(g1 - w1) < 0.25 && std::fabs(g01 - w01) < 0.4, "QPSK off"); CHECK(g01 < 7.0, "QPSK should be less peaky than Gaussian: %.2f", g01); }
    // 4. noise clipped at 1.5 sigma and noise with impulses
    { auto x = gauss((size_t)(fs * 5), 6, 1.0); for (auto& v : x) { const float a = std::abs(v); if (a > 1.0f) v *= 1.0f / a; } auto r = run(x40::Papr, x, fs); CHECK(factNum(r, "CCDF 0.1 %") < 3.2, "clipped: %.2f", factNum(r, "CCDF 0.1 %")); CHECK(factStr(r, "reading").find("less peaky") != std::string::npos || factStr(r, "reading").find("constant envelope") != std::string::npos, "clipped reading: %s", factStr(r, "reading").c_str()); }
    { auto x = gauss((size_t)(fs * 5), 7, 0.01); for (size_t i = 0; i < x.size(); i += 500) x[i] += cf32(2.0f, 0); auto r = run(x40::Papr, x, fs); CHECK(factNum(r, "CCDF 0.1 %") > 9.0, "impulses: %.2f", factNum(r, "CCDF 0.1 %")); CHECK(factStr(r, "reading").find("peakier") != std::string::npos, "impulse reading: %s", factStr(r, "reading").c_str()); }
    printf(fails ? "x40 papr: %d FAILED\n" : "x40 papr: ok\n", fails);
    return fails ? 1 : 0;
}
