// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FM census: stations at several offsets, levels and tuning errors in one capture, noise over all of it; every one must come out with its own PI and name.
#include "dect2/fm/fm_census.h"
#include "dect2/fm/fm_gen.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Tx { double mhz; float dB; double cfo; bool stereo; int pi; const char* ps; };

static void one(double rate, double centreMhz, double secs, const std::vector<Tx>& txs, double noiseDb, double spurOff) {
    const size_t n = (size_t)(secs * rate);
    std::vector<cf32> x(n, cf32(0, 0));
    for (const Tx& t : txs) {
        FmGenConfig c; c.rate = rate; c.cnrDb = 200; c.cfoHz = (t.mhz - centreMhz) * 1e6 + t.cfo; c.stereo = t.stereo; c.pi = t.pi; c.ps = t.ps;
        FmGenerator g(c);
        std::vector<cf32> y; g.generate(n, y);
        const float a = (float)std::pow(10.0, t.dB / 20);
        for (size_t i = 0; i < n; i++) x[i] += y[i] * a;
    }
    std::mt19937 r(1); std::normal_distribution<float> gn(0, (float)std::pow(10.0, noiseDb / 20) * 0.7071f);
    for (auto& v : x) v += cf32(gn(r), gn(r));
    for (size_t i = 0; i < n; i++) x[i] += 0.02f;            // the radio's DC spike
    if (spurOff != 0) for (size_t i = 0; i < n; i++) { const double w = 2 * M_PI * spurOff * (double)i / rate; x[i] += cf32((float)std::cos(w), (float)std::sin(w)) * 0.05f; }
    FmCensus cen;
    CHECK(cen.configure(rate, centreMhz * 1e6), "configure");
    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < n; i += 50000) cen.feed(x.data() + i, std::min<size_t>(50000, n - i));
    const double used = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const auto rows = cen.rows();
    printf("rate %.1f Msps, %zu stations sent: %zu rows, %.2f s of CPU per %.1f s of signal\n", rate / 1e6, txs.size(), rows.size(), used, secs);
    for (const Tx& t : txs) {
        const FmCensusRow* f = nullptr;
        for (const auto& w : rows) if (std::fabs(w.mhz - t.mhz) < 0.01) f = &w;
        CHECK(f, "%.1f MHz not found", t.mhz);
        if (!f) continue;
        printf("  %6.1f: PI %04X '%s' level %.1f above %.1f snr %.0f stereo %d rds %d\n", f->mhz, f->pi, f->ps.c_str(), f->levelDb, f->aboveDb, f->snrDb, f->stereo, f->rds);
        CHECK(f->pi == t.pi && f->ps == t.ps, "%.1f MHz: PI %04X '%s', wanted %04X '%s'", t.mhz, f->pi, f->ps.c_str(), t.pi, t.ps);
        CHECK(f->stereo == t.stereo, "%.1f MHz: stereo %d", t.mhz, f->stereo);
    }
    CHECK(rows.size() == txs.size(), "%zu rows for %zu stations (a false station)", rows.size(), txs.size());
}

int main() {
    const std::vector<Tx> a = {
        {98.3, -8, 0, true, 0x5001, "ALPHA   "}, {98.7, -20, 1500, true, 0x5002, "BRAVO   "}, {99.2, -32, -2500, false, 0x5003, "CHARLIE "},
        {99.6, -14, 400, true, 0x5004, "DELTA   "}, {100.1, -26, -900, true, 0x5005, "ECHO    "}, {100.9, -18, 2000, false, 0x5006, "FOXTROT "}};
    one(4e6, 99.5, 8, a, -62, 0);                // an integer ratio to the 500 kHz
    one(3.2e6, 99.65, 8, a, -62, 850e3);          // another ratio and an unmodulated spur (not a station)
    // two strong neighbours on the 200 kHz raster and a weak one
    one(4e6, 100.0, 8, {{99.7, -10, 0, true, 0x6001, "ONE     "}, {99.9, -12, 0, true, 0x6002, "TWO     "}, {100.8, -40, 0, true, 0x6003, "THREE   "}}, -66, 0);
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
