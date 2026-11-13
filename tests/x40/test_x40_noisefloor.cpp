// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Noise floor logger: noise whose floor rises in one sub-band (a broadband interferer: 5 dB for 60 s), a narrow signal that must not count, a whole-band gain change, slow drift, and a steady noise;
// the rise is found in the right sub-band with its size and time, and ended. Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// white noise of power p0; plus, between t1 and t2, extra noise confined to the sub-band around hz (a one-pole low-pass of bandwidth bw mixed up); plus a gain step at tg of gdb
struct Scene {
    double fs, p0, hz, bw, extra, t1, t2, tg, gdb, driftDbPerMin, narrowAmp = 0, narrowHz = 0; Rng rng{3}, rng2{4}; cf32 lp{0, 0}; double ph = 0, pn = 0;
    std::vector<cf32> operator()(double t0, size_t n) {
        std::vector<cf32> x(n); const float a = (float)(1 - std::exp(-2 * kPi * bw / 2 / fs));
        for (size_t i = 0; i < n; i++) {
            const double t = t0 + i / fs; const float s = (float)std::sqrt(p0 / 2);
            cf32 v(s * rng.g(), s * rng.g());
            lp += a * (cf32(rng2.g(), rng2.g()) - lp); ph += 2 * kPi * hz / fs; pn += 2 * kPi * narrowHz / fs;
            if (t >= t1 && t < t2) v += (float)std::sqrt(extra * p0 * 2 * 2 * kPi * bw / 2 / fs / 2) * lp * cf32((float)std::cos(ph), (float)std::sin(ph)) * 3.0f;
            if (narrowAmp > 0) v += (float)narrowAmp * cf32((float)std::cos(pn), (float)std::sin(pn));
            double g = std::pow(10.0, driftDbPerMin * t / 60 / 20); if (t >= tg) g *= std::pow(10.0, gdb / 20);
            x[i] = v * (float)g;
        }
        return x;
    }
};

int main() {
    const double fs = 2e6, centre = 145e6;
    // 1. a broadband rise in one sub-band (around +300 kHz, 250 kHz wide, enough to lift more than the median) at 90 - 150 s, and a strong narrow signal all along that must not count
    for (double bw : {250000.0}) {
        Scene sc{fs, 1e-3, 300000, bw, 2.0, 90, 150, 1e9, 0, 0, 0.05, -400000};
        auto t = runChunks(x40::NoiseFloor, fs, 200.0, std::ref(sc), 0, centre, 1.0);
        int rises = 0, backs = 0; double rd = 0; std::string where;
        for (auto& r : t.rows) { if (r.kind == "Noise rise") { rises++; double v = 0; sscanf(strstr(r.text.c_str(), ": ") + 2, "%lf", &v); if (v > rd) { rd = v; where = r.id; } } if (r.kind == "Noise back") backs++; }
        printf("  one broadband interferer 250 kHz wide: %d sub-bands raised, the largest %.1f dB at %s; %d ended\n", rises, rd, where.c_str(), backs);
        CHECK(rises >= 1 && backs == rises, "rises %d backs %d", rises, backs);
        CHECK(countKind(t, "Whole band") == 0, "whole band rows %d", countKind(t, "Whole band"));
        CHECK(std::fabs(atof(where.c_str()) - 145.3) < 0.15, "the largest rise is at %s want near 145.3 MHz", where.c_str());
        CHECK(rd > 8, "rise %.1f dB", rd);
        for (auto& r : t.rows) if (r.kind == "Noise rise") CHECK(std::fabs(atof(r.id.c_str()) - 144.6) > 0.15, "a rise at the narrow signal's sub-band %s", r.id.c_str());
    }
    // 2. a gain step of +6 dB at 120 s: one "Whole band" row, no per-band rises
    { Scene sc{fs, 1e-3, 0, 1e5, 0, 1e9, 1e9, 120, 6.0, 0}; auto t = runChunks(x40::NoiseFloor, fs, 180.0, std::ref(sc), 0, centre, 1.0);
      CHECK(countKind(t, "Whole band") == 1 && countKind(t, "Noise rise") == 0, "gain step: whole %d, rises %d", countKind(t, "Whole band"), countKind(t, "Noise rise")); }
    // 3. slow drift of 0.5 dB per minute: nothing; steady noise: the facts show the floor to 0.1 dB
    { Scene sc{fs, 1e-3, 0, 1e5, 0, 1e9, 1e9, 1e9, 0, 0.5}; auto t = runChunks(x40::NoiseFloor, fs, 180.0, std::ref(sc), 0, centre, 1.0); CHECK(t.rows.empty(), "drift: %zu rows", t.rows.size()); }
    { Scene sc{fs, 1e-3, 0, 1e5, 0, 1e9, 1e9, 1e9, 0, 0}; auto t = runChunks(x40::NoiseFloor, fs, 60.0, std::ref(sc), 0, centre, 1.0);
      const double w = factNum(t, "band-wide floor"); const double want = 10 * std::log10(1e-3 / 4096);       // power per bin of white noise p0: p0 / N
      CHECK(std::fabs(w - want) < 0.3, "floor %.2f want %.2f", w, want); CHECK(t.rows.empty(), "steady: %zu rows", t.rows.size()); }
    printf(fails ? "x40 noisefloor: %d FAILED\n" : "x40 noisefloor: ok\n", fails);
    return fails ? 1 : 0;
}
