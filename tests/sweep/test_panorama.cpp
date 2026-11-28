// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Panorama: a 10 MHz span from a 2.4 Msps fake radio, stitched from several steps, with tones whose place in the strip is checked.
#include "dect2/sweep/panorama.h"
#include "dect2/common/stable_random.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
using namespace dect2::sweep;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

class FakeRadio : public SweepSource {
public:
    double rate = 2.4e6;
    std::vector<std::pair<double, double>> tones;   // Hz, amplitude
    bool retune(double hz, std::string&) override { fc_ = hz; return true; }
    size_t capture(cf32* out, size_t n) override {
        dect2::normal_distribution<float> g(0.f, 0.005f);
        for (size_t i = 0; i < n; i++, t_++) {
            float re = g(rng_), im = g(rng_);
            for (auto& tn : tones) {
                if (std::fabs(tn.first - fc_) > 0.45 * rate) continue;
                const double ph = 2 * M_PI * std::fmod((tn.first - fc_) * (double)t_ / rate, 1.0);
                re += (float)(tn.second * std::cos(ph)); im += (float)(tn.second * std::sin(ph));
            }
            out[i] = cf32(re, im);
        }
        return n;
    }
    double sampleRate() const override { return rate; }
private:
    double fc_ = 0; uint64_t t_ = 0; std::mt19937 rng_{7};
};

int main() {
    // the config
    const SweepConfig c = panoramaConfig(100e6, 10e6);
    CHECK(std::fabs(c.startHz - 95e6) < 1 && std::fabs(c.stopHz - 105e6) < 1, "range %.0f..%.0f", c.startHz, c.stopHz);
    CHECK(std::fabs(c.cropFrac - 0.10) < 1e-9, "crop %.2f", c.cropFrac);
    CHECK(panoramaConfig(1e6, 20e6).startHz > 0, "start must stay above 0");
    const int steps = panoramaSteps(10e6, 2.4e6);
    CHECK(steps >= 5 && steps <= 8, "steps at 2.4 Msps: %d", steps);   // 10 MHz / (2.4 MHz * 0.8 kept) ~ 5.2 -> 6
    CHECK(panoramaSteps(10e6, 0) == 0, "no rate");
    CHECK(panoramaSteps(20e6, 2.4e6) > panoramaSteps(5e6, 2.4e6), "a wider span needs more steps");

    // the sweep: tones across the span, in different steps and near a step boundary
    FakeRadio r;
    r.tones = {{95.8e6, 0.1}, {99.2e6, 0.1}, {100.0e6, 0.1}, {103.7e6, 0.1}, {104.6e6, 0.1}};
    Sweep s; std::string err;
    CHECK(runSweep(r, c, s, err), "sweep: %s", err.c_str());
    CHECK(s.hops == steps, "hops %d vs %d", s.hops, steps);
    const double covered = s.bins() * s.binHz;
    CHECK(covered >= 10e6 - 2 * s.binHz && covered <= 10e6 + 2 * s.binHz, "stitched width %.3f MHz", covered / 1e6);
    CHECK(std::fabs(s.startHz - 95e6) < 1, "start %.1f", s.startHz);
    CHECK(s.bins() >= 900 && s.bins() <= 1100, "bins %zu", s.bins());

    const int cols = 500;
    const auto col = panoramaColumns(s, cols);
    CHECK((int)col.size() == cols, "columns %zu", col.size());
    // noise level of the columns, then each tone must stand well above it in exactly its column (+-1)
    std::vector<float> sorted(col); std::sort(sorted.begin(), sorted.end());
    const float noise = sorted[sorted.size() / 2];
    for (auto& tn : r.tones) {
        const int want = (int)((tn.first - s.startHz) / (s.bins() * s.binHz) * cols);
        int best = 0; for (int i = 0; i < cols; i++) if (col[i] > col[best]) { /* global max below */ }
        float mx = -1e9f; int at = -1;
        for (int i = std::max(0, want - 2); i <= std::min(cols - 1, want + 2); i++) if (col[i] > mx) { mx = col[i]; at = i; }
        (void)best;
        CHECK(mx > noise + 20, "tone %.2f MHz: %.1f dB vs noise %.1f", tn.first / 1e6, mx, noise);
        CHECK(std::abs(at - want) <= 1, "tone %.2f MHz at column %d, wanted %d", tn.first / 1e6, at, want);
    }
    // no bin falls into an empty column gap: every column is filled
    for (int i = 0; i < cols; i++) CHECK(col[i] > -149.f, "column %d empty", i);
    CHECK(panoramaColumns(Sweep(), 10).size() == 10 && panoramaColumns(Sweep(), 10)[0] == -150.f, "empty sweep");

    printf(fails ? "FAILED %d\n" : "panorama OK\n", fails);
    return fails ? 1 : 0;
}
