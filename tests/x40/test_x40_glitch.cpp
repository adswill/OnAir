// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SDR stream glitch detector: a steady tone with noise, then samples removed (a drop of 1 to 6 samples), samples repeated, a run of zeros, a repeated block of 256 samples and a gain step, at known times;
// the events and their times, the number of samples lost, and no false events on a clean stream, with a slowly drifting tone and with noise only (where a drop cannot be seen). Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<cf32> tone(size_t n, double fs, double hz, double amp, double noise, unsigned seed, double drift = 0) {
    Rng r(seed); std::vector<cf32> x(n); double ph = 0; const float sg = (float)std::sqrt(noise / 2);
    for (size_t i = 0; i < n; i++) { ph += 2 * kPi * (hz + drift * i / fs) / fs; x[i] = (float)amp * cf32((float)std::cos(ph), (float)std::sin(ph)) + cg(r, sg, sg); }
    return x;
}
static std::vector<std::string> rowsOf(const X40Telemetry& t, const char* kind) { std::vector<std::string> v; for (auto& r : t.rows) if (r.kind == kind) v.push_back(r.id + " | " + r.text); return v; }

int main() {
    const double fs = 2e6, hz = 123456.0;
    // 1. drops of 1, 2, 3, 5 samples and repeats of 1, 2 samples, at 1.0, 1.5, 2.0, 2.5, 3.0, 3.5 s
    for (double f : {123456.0, -45678.0}) {
        auto x = tone((size_t)(fs * 5), fs, f, 1.0, 1e-4, 3);
        struct Ev { double t; int n; };                   // n > 0: samples removed; n < 0: samples repeated
        const Ev ev[] = {{3.5, -2}, {3.0, -1}, {2.5, 5}, {2.0, 3}, {1.5, 2}, {1.0, 1}};      // later first, so earlier indices stay valid
        for (auto& e : ev) { const size_t i = (size_t)(e.t * fs); if (e.n > 0) x.erase(x.begin() + (long)i, x.begin() + (long)i + e.n); else { std::vector<cf32> rep(x.begin() + (long)i, x.begin() + (long)i + (-e.n)); x.insert(x.begin() + (long)i, rep.begin(), rep.end()); } }
        auto t = run(x40::Glitch, x, fs);
        const auto drops = rowsOf(t, "Dropped samples"), reps = rowsOf(t, "Repeated samples");
        for (auto& s : drops) printf("   %s\n", s.c_str()); for (auto& s : reps) printf("   %s\n", s.c_str());
        CHECK(drops.size() == 4 && reps.size() == 2, "tone %.0f: %zu drops, %zu repeats (want 4 and 2)", f, drops.size(), reps.size());
        // the sizes in time order
        std::vector<std::pair<double, int>> got; for (auto& s : drops) got.push_back({atof(s.c_str()), atoi(strstr(s.c_str(), "about ") + 6)}); std::sort(got.begin(), got.end());
        const int want[4] = {1, 3, 5, 3}; (void)want;
        if (got.size() == 4) { CHECK(got[0].second == 1 && got[1].second == 2 && got[2].second == 3 && got[3].second == 5, "drop sizes %d %d %d %d want 1 2 3 5", got[0].second, got[1].second, got[2].second, got[3].second);
            const double tt[4] = {1.0, 1.5, 2.0, 2.5}; for (int i = 0; i < 4; i++) CHECK(std::fabs(got[(size_t)i].first - tt[i]) < 0.001, "drop time %.4f want %.1f", got[(size_t)i].first, tt[i]); }
    }
    // 2. zero run, repeated block, a +3 dB gain step at 1 s intervals
    { auto x = tone((size_t)(fs * 4), fs, hz, 1.0, 1e-4, 4);
      for (size_t i = (size_t)(1.0 * fs); i < (size_t)(1.0 * fs) + 40; i++) x[i] = cf32(0, 0);                                                    // a run of 40 zeros
      { const size_t i = (size_t)(2.0 * fs); for (int k = 0; k < 256; k++) x[i + 256 + (size_t)k] = x[i + (size_t)k]; }                           // block 2 = block 1
      for (size_t i = (size_t)(3.0 * fs); i < x.size(); i++) x[i] *= 1.4125f;                                                                     // +3 dB
      auto t = run(x40::Glitch, x, fs);
      CHECK(rowsOf(t, "Zero run").size() == 1, "zero runs %zu", rowsOf(t, "Zero run").size());
      CHECK(rowsOf(t, "Repeated block").size() == 1, "repeated blocks %zu", rowsOf(t, "Repeated block").size());
      const auto st = rowsOf(t, "Level step"); CHECK(st.size() == 1 && std::fabs(atof(st[0].c_str()) - 3.0) < 0.3, "level steps %zu (%s)", st.size(), st.empty() ? "" : st[0].c_str());
      CHECK(!st.empty() && std::fabs(atof(strstr(st[0].c_str(), "| at ") ? strstr(st[0].c_str(), "| at ") + 5 : "0") - 3.0) < 0.01, "step time"); }
    // 3. a clean stream with a drifting tone (50 Hz/s) and with a weak tone: no events; noise only: no events, no tone watched
    { auto x = tone((size_t)(fs * 6), fs, hz, 1.0, 1e-4, 5, 50.0); auto t = run(x40::Glitch, x, fs); CHECK(t.rows.empty(), "drifting tone: %zu rows", t.rows.size()); }
    { auto x = tone((size_t)(fs * 6), fs, hz, 0.05, 1e-3, 6); auto t = run(x40::Glitch, x, fs); CHECK(t.rows.empty(), "weak tone: %zu rows", t.rows.size()); }
    { ToneScene sc(fs, 1.0, 3); auto t = runChunks(x40::Glitch, fs, 6.0, std::ref(sc), 0, 145e6, 0.5); CHECK(t.rows.empty(), "noise: %zu rows", t.rows.size()); CHECK(factStr(t, "watched tone").rfind("none", 0) == 0, "noise: tone %s", factStr(t, "watched tone").c_str()); }
    printf(fails ? "x40 glitch: %d FAILED\n" : "x40 glitch: ok\n", fails);
    return fails ? 1 : 0;
}
