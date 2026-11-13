// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Burst finder: a periodic key-fob style burst (433.92 MHz area), an irregular narrow burst, a wide burst (a Wi-Fi-like 400 kHz) and a fast burst, in a 2 Msps and a 4 Msps capture with the radio's offset;
// counts, frequencies, lengths, bandwidth class and the repetition period against the generator; a noise sweep; the false bursts on noise alone. Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct B { double t, dur, hz, bw, amp; };
struct Scene {
    double fs, noise; std::vector<B> bursts; Rng rng{4}; std::vector<double> ph;
    std::vector<cf32> operator()(double t0, size_t n) {
        std::vector<cf32> x(n); const float sg = (float)std::sqrt(noise / 2);
        for (auto& v : x) v = cf32(sg * rng.g(), sg * rng.g());
        if (ph.size() < bursts.size()) ph.assign(bursts.size(), 0.0);
        for (size_t k = 0; k < bursts.size(); k++) {
            const B& b = bursts[k]; if (b.t + b.dur < t0 || b.t > t0 + n / fs) continue;
            const size_t i0 = (size_t)std::max(0.0, (b.t - t0) * fs), i1 = (size_t)std::min((double)n, (b.t + b.dur - t0) * fs);
            const int nt = b.bw > 0 ? (int)(b.bw / 4000) : 1;
            for (size_t i = i0; i < i1; i++) {
                const double t = t0 + i / fs; cf32 v(0, 0);
                for (int m = 0; m < nt; m++) { const double f = b.hz + (nt > 1 ? (m - (nt - 1) / 2.0) * b.bw / nt : 0); const double p = 2 * kPi * f * t + 1.7 * m * m; v += cf32((float)std::cos(p), (float)std::sin(p)); }
                x[i] += (float)(b.amp / nt) * v * (float)(nt > 1 ? std::sqrt((double)nt) : 1.0);
            }
        }
        return x;
    }
};
static int countNear(const X40Telemetry& t, double mhz, double tolHz, const char* wide) {
    int n = 0; for (auto& r : t.rows) if (r.kind == "Burst" && std::fabs(atof(r.id.c_str()) * 1e6 - mhz * 1e6) < tolHz && (!wide || r.text.find(wide) != std::string::npos)) n++; return n;
}
static double lenOf(const std::string& s) { const char* p = strstr(s.c_str(), "length "); return p ? atof(p + 7) : -1; }

int main() {
    for (double fs : {2e6, 4e6}) for (double lo : {0.0, 2500.0}) {
        const double centre = 433.92e6; Scene sc{fs, noiseFor(30, 25000, fs)};
        // fob: 24 bursts of 3.2 ms every 0.25 s (0.1 % clock jitter) at +120 kHz; sensor: 9 irregular bursts of 1.1 ms at -310 kHz; wide: 6 bursts of 1 ms, 400 kHz wide at +500 kHz; fast: 5 bursts of 0.35 ms at -700 kHz
        Rng r(2); for (int i = 0; i < 24; i++) sc.bursts.push_back({0.2 + 0.25 * i * (1 + 0.001 * r.g()), 0.0032, 120000 + lo, 0, 1.0});
        const double ts[9] = {0.7, 1.1, 1.9, 2.0, 3.3, 3.7, 4.4, 5.0, 5.4}; for (double t : ts) sc.bursts.push_back({t, 0.0011, -310000 + lo, 0, 1.0});
        for (int i = 0; i < 6; i++) sc.bursts.push_back({0.5 + 0.9 * i, 0.001, 500000 + lo, 400000, 3.0});
        for (int i = 0; i < 5; i++) sc.bursts.push_back({1.3 + 1.1 * i, 0.00035, -700000 + lo, 0, 1.0});
        auto t = runChunks(x40::Burst, fs, 6.5, std::ref(sc), 0, centre, 0.5);
        const double tol = 12000;
        CHECK(countNear(t, (centre + 120000 + lo) / 1e6, tol, nullptr) == 24, "fs %.0f lo %.0f: fob bursts %d want 24", fs, lo, countNear(t, (centre + 120000 + lo) / 1e6, tol, nullptr));
        CHECK(countNear(t, (centre - 310000 + lo) / 1e6, tol, nullptr) == 9, "sensor bursts %d want 9", countNear(t, (centre - 310000 + lo) / 1e6, tol, nullptr));
        CHECK(countNear(t, (centre + 500000 + lo) / 1e6, 60000, "wide") == 6, "wide bursts %d want 6", countNear(t, (centre + 500000 + lo) / 1e6, 60000, "wide"));
        CHECK(countNear(t, (centre - 700000 + lo) / 1e6, tol, nullptr) >= 4, "fast bursts %d want 5", countNear(t, (centre - 700000 + lo) / 1e6, tol, nullptr));
        CHECK(countNear(t, (centre + 120000 + lo) / 1e6, tol, "narrow") == 24, "fob not all narrow");
        // lengths
        double sum = 0; int c = 0; for (auto& rw : t.rows) if (rw.kind == "Burst" && std::fabs(atof(rw.id.c_str()) * 1e6 - (centre + 120000 + lo)) < tol) { sum += lenOf(rw.text); c++; }
        const double mean = c ? sum / c : 0; printf("  fs %.0f lo %.0f: fob length mean %.2f ms (true 3.20), sensor/wide/fast %d/%d/%d\n", fs, lo, mean, countNear(t, (centre - 310000 + lo) / 1e6, tol, nullptr), countNear(t, (centre + 500000 + lo) / 1e6, 60000, "wide"), countNear(t, (centre - 700000 + lo) / 1e6, tol, nullptr));
        CHECK(std::fabs(mean - 3.2) < 0.4, "fob length %.2f ms want 3.2", mean);
        // repetition
        bool rep = false; for (auto& rw : t.rows) if (rw.kind == "Repeating" && std::fabs(atof(rw.id.c_str()) * 1e6 - (centre + 120000 + lo)) < tol) { const double p = atof(strstr(rw.text.c_str(), "every ") + 6); rep = std::fabs(p - 0.25) < 0.005; }
        CHECK(rep, "fs %.0f: the 0.25 s repetition not reported", fs);
        // the irregular sensor must NOT be called repeating
        for (auto& rw : t.rows) if (rw.kind == "Repeating") CHECK(std::fabs(atof(rw.id.c_str()) * 1e6 - (centre - 310000 + lo)) > tol, "irregular bursts called repeating");
    }
    // sweep: the burst amplitude against the noise; bursts found of 10 (2 ms, narrow); false bursts
    printf("burst to noise in 25 kHz -> found of 10, false:\n");
    for (double cnr : {30.0, 20.0, 14.0, 10.0, 7.0, 4.0}) {
        Scene sc{2e6, noiseFor(cnr, 25000, 2e6)}; for (int i = 0; i < 10; i++) sc.bursts.push_back({0.3 + 0.4 * i, 0.002, 200000, 0, 1.0});
        auto t = runChunks(x40::Burst, 2e6, 4.5, std::ref(sc), 0, 433.92e6, 0.5);
        const int ok = countNear(t, 433.92e6 / 1e6 + 0.2, 12000, nullptr); const int tot = countKind(t, "Burst");
        printf("  %.0f dB: %d, %d\n", cnr, ok, tot - ok);
        if (cnr >= 14) { CHECK(ok == 10, "%.0f dB: found %d", cnr, ok); CHECK(tot == ok, "%.0f dB: %d extra", cnr, tot - ok); }
    }
    { Scene sc{2e6, 1.0}; auto t = runChunks(x40::Burst, 2e6, 120.0, std::ref(sc), 0, 433.92e6, 1.0); printf("noise only, 120 s: %d false bursts\n", countKind(t, "Burst")); CHECK(countKind(t, "Burst") == 0, "false bursts %d", countKind(t, "Burst")); }
    printf(fails ? "x40 burst: %d FAILED\n" : "x40 burst: ok\n", fails);
    return fails ? 1 : 0;
}
