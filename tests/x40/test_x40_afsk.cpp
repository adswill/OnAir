// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AFSK tone tuner: Bell 202 packet bursts (random bits, continuous-phase 1200 / 2200 Hz audio, NRZI-like random mix) on an FM carrier with known deviation, tone level difference (twist) and clock error; the mark
// and space frequencies, twist, peak deviation, burst length and count are compared with the generator; noise sweep; false bursts on noise, speech-like audio and a steady tone. Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Tnc {
    double fs, dev, twistDb, baud, ppm, lo, noise, period, burstLen; unsigned seed = 3; Rng rng{5}; std::mt19937 g{7}; double aph = 0, ph = 0; int bit = 0; double tNextBit = 0;
    std::vector<cf32> operator()(double t0, size_t n) {
        std::vector<cf32> x(n); const float sg = (float)std::sqrt(noise / 2);
        const double aMark = 1.0, aSpace = std::pow(10.0, twistDb / 20), norm = std::max(aMark, aSpace);
        for (size_t i = 0; i < n; i++) {
            const double t = t0 + i / fs; const double u = std::fmod(t, period); const bool on = t >= 0.5 && u < burstLen;
            double m = 0;
            if (on) { if (t >= tNextBit) { bit ^= (g() & 1); tNextBit = t + 1.0 / (baud * (1 + ppm * 1e-6)); } const double f = bit ? 2200.0 : 1200.0; aph += 2 * kPi * f / fs; m = (bit ? aSpace : aMark) / norm * std::sin(aph); }
            ph += 2 * kPi * (lo + dev * m) / fs;
            x[i] = cf32((float)std::cos(ph), (float)std::sin(ph)) + cg(rng, sg, sg);
        }
        return x;
    }
};
static std::vector<std::string> bursts(const X40Telemetry& t) { std::vector<std::string> v; for (auto& r : t.rows) if (r.kind == "Burst") v.push_back(r.text); return v; }

int main() {
    const struct { double fs, dev, twist, ppm, lo; } cs[] = {{48000, 3000, 0, 0, 0}, {48000, 2500, -4, 300, 1500}, {96000, 3500, 3, -400, -2000}, {250000, 2000, -6, 0, 3000}};
    for (auto& c : cs) {
        Tnc tx{c.fs, c.dev, c.twist, 1200, c.ppm, c.lo, noiseFor(30, 16000, c.fs), 2.0, 0.5};
        auto t = runChunks(x40::Afsk, c.fs, 14.0, std::ref(tx), 0, 144.39e6, 1.0);
        const auto b = bursts(t);
        CHECK(b.size() >= 5 && b.size() <= 7, "fs %.0f: %zu bursts want 6", c.fs, b.size());
        const double mk = factNum(t, "mark tone"), sp = factNum(t, "space tone"), tw = factNum(t, "twist (space over mark)"), pk = factNum(t, "peak deviation of the burst") * 1000;
        CHECK(std::fabs(mk - 1200) < 25 && std::fabs(sp - 2200) < 70, "tones %.0f / %.0f", mk, sp);
        CHECK(std::fabs(tw - c.twist) < 1.5, "twist %.1f want %.0f", tw, c.twist);
        CHECK(std::fabs(pk - c.dev) < 0.16 * c.dev, "peak %.0f want %.0f", pk, c.dev);
        double len = 0; if (!b.empty()) len = atof(b.back().c_str()); CHECK(std::fabs(len - 0.5) < 0.12, "burst length %.2f want 0.5", len);
        printf("  fs %.0f: %zu bursts, mark %.0f space %.0f twist %+.1f (put in %+.0f) peak %.0f Hz (put in %.0f)\n", c.fs, b.size(), mk, sp, tw, c.twist, pk, c.dev);
    }
    printf("noise sweep (carrier to noise in 16 kHz): bursts found of 6, twist read (true -4 dB):");
    for (double cnr : {30.0, 20.0, 15.0, 12.0, 10.0}) { Tnc tx{48000, 3000, -4, 1200, 0, 0, noiseFor(cnr, 16000, 48000), 2.0, 0.5}; auto t = runChunks(x40::Afsk, 48000, 14.0, std::ref(tx), 0, 144.39e6, 1.0);
        printf(" %.0f: %zu/%.1f", cnr, bursts(t).size(), factNum(t, "twist (space over mark)")); if (cnr >= 15) CHECK(bursts(t).size() >= 5 && std::fabs(factNum(t, "twist (space over mark)") + 4) < 2.0, "%.0f dB: %zu", cnr, bursts(t).size()); }
    printf("\n");
    { ToneScene sc(48000, 1.0, 3); auto t = runChunks(x40::Afsk, 48000, 20.0, std::ref(sc), 0, 144.39e6, 1.0); CHECK(bursts(t).empty(), "noise: %zu bursts", bursts(t).size()); }
    { Tnc tx{48000, 3000, 0, 1200, 0, 0, noiseFor(30, 16000, 48000), 1e9, 1e9}; tx.period = 1e9; tx.burstLen = 1e9;     // 1200 Hz tone only, for the whole run (a steady tone: no burst edges)
      auto t = runChunks(x40::Afsk, 48000, 20.0, std::ref(tx), 0, 144.39e6, 1.0); CHECK(bursts(t).size() <= 1, "a steady AFSK stream: %zu bursts", bursts(t).size()); }
    printf(fails ? "x40 afsk: %d FAILED\n" : "x40 afsk: ok\n", fails);
    return fails ? 1 : 0;
}
