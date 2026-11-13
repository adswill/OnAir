// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FM SINAD meter: SINAD, deviation, THD and carrier offset against a generated FM signal (1 kHz tone, 3 kHz deviation) with known distortion and noise, against the FM noise theory
// (the noise in the demodulated 300 - 3000 Hz band is N0 (f2^3 - f1^3) / (3 A^2) in Hz^2), at three input rates, with a carrier offset and drift; the 12 dB crossing; noise alone. Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct FmSrc {
    double fs, dev, tone, lo, drift, noise, h2 = 0, h3 = 0; Rng rng{5}; double ph = 0;
    std::vector<cf32> operator()(double t0, size_t n) {
        std::vector<cf32> x(n); const float sg = (float)std::sqrt(noise / 2);
        for (size_t i = 0; i < n; i++) {
            const double t = t0 + i / fs, w = 2 * kPi * tone * t;
            const double m = std::sin(w) + h2 * std::sin(2 * w + 0.4) + h3 * std::sin(3 * w + 1.1);              // the audio, with its distortion
            ph += 2 * kPi * (lo + drift * t + dev * m) / fs;
            x[i] = cf32((float)std::cos(ph), (float)std::sin(ph)) + cg(rng, sg, sg);
        }
        return x;
    }
};
static double sinadOf(const X40Telemetry& t) { return factNum(t, "SINAD"); }
// theory: tone power dev^2 / 2; noise N0 (f2^3 - f1^3) / 3 with N0 = noisePower / fs (carrier amplitude 1)
static double theory(double dev, double noisePower, double fs) { const double S = dev * dev / 2, N = noisePower / fs * (std::pow(3000.0, 3) - std::pow(300.0, 3)) / 3; return 10 * std::log10((S + N) / N); }

int main() {
    // 1. three rates, offsets and drift: SINAD against the theory at carrier to noise ratios of 30 and 20 dB in 16 kHz
    for (double fs : {48000.0, 96000.0, 250000.0}) for (double cnr : {30.0, 20.0}) {
        const double off = fs == 48000 ? 3000.0 : fs == 96000 ? -1500.0 : 2500.0, noise = noiseFor(cnr, 16000, fs);
        FmSrc f{fs, 3000, 1000, off, 5.0, noise}; auto t = runChunks(x40::Sinad, fs, 10.0, std::ref(f), 0, 146.52e6, 1.0);
        const double want = theory(3000, noise, fs), got = sinadOf(t);
        CHECK(std::fabs(got - want) < 1.5, "fs %.0f cnr %.0f: SINAD %.1f want %.1f (%s)", fs, cnr, got, want, t.headline.c_str());
        CHECK(std::fabs(factNum(t, "peak deviation") - 3.0) < 0.1, "deviation %.2f", factNum(t, "peak deviation"));
        CHECK(std::fabs(factNum(t, "test tone") - 1000.0) < 1.0, "tone %.1f", factNum(t, "test tone"));
        CHECK(std::fabs(factNum(t, "carrier offset") - (off + 5.0 * 5)) < 60, "offset %.0f", factNum(t, "carrier offset"));
    }
    // 2. distortion: 2nd harmonic 2 %, 3rd 1 % of the audio at high carrier to noise: THD about 2.24 %
    { FmSrc f{48000, 3000, 1000, 0, 0, noiseFor(60, 16000, 48000), 0.02, 0.01}; auto t = runChunks(x40::Sinad, 48000, 8.0, std::ref(f), 0, 146.52e6, 1.0);
      const double thd = factNum(t, "THD"); CHECK(std::fabs(thd - 2.24) < 0.2, "THD %.2f want 2.24", thd);
      const double want = -20 * std::log10(std::sqrt(0.02 * 0.02 + 0.01 * 0.01)); CHECK(std::fabs(sinadOf(t) - want) < 1.5, "distortion-limited SINAD %.1f want %.1f", sinadOf(t), want); }
    // 3. sweep through the 12 dB point: carrier to noise in 16 kHz -> SINAD read (theory), the crossing row
    printf("FM noise sweep (carrier to noise in 16 kHz -> SINAD read / theory, dB):");
    for (double cnr : {25.0, 18.0, 14.0, 10.0, 8.0, 6.0}) {
        const double noise = noiseFor(cnr, 16000, 48000); FmSrc f{48000, 3000, 1000, 0, 0, noise}; auto t = runChunks(x40::Sinad, 48000, 10.0, std::ref(f), 0, 146.52e6, 1.0);
        printf(" %.0f: %.1f/%.1f", cnr, sinadOf(t), theory(3000, noise, 48000));
        if (cnr >= 10) CHECK(std::fabs(sinadOf(t) - theory(3000, noise, 48000)) < 2.0, "%.0f dB: %.1f vs %.1f", cnr, sinadOf(t), theory(3000, noise, 48000));
    }
    printf("\n");
    // 4. the deviation falls during the run (a mis-set microphone gain): SINAD falls through 12 dB at 8 s and the crossing is logged once
    { const double noise = noiseFor(14, 16000, 48000); printf("crossing test: theory %.1f dB then %.1f dB\n", theory(3000, noise, 48000), theory(700, noise, 48000));
      CHECK(theory(3000, noise, 48000) > 12 && theory(700, noise, 48000) < 11, "test set-up");
      static FmSrc f{48000, 3000, 1000, 0, 0, 1e-4}; f = FmSrc{48000, 3000, 1000, 0, 0, noise};
      auto gen = [&](double t0, size_t n) { f.dev = t0 < 8 ? 3000 : 700; return f(t0, n); };
      auto t = runChunks(x40::Sinad, 48000, 18.0, gen, 0, 146.52e6, 1.0); int cross = 0; for (auto& r : t.rows) if (r.kind == "SINAD" && r.text.rfind("under 12", 0) == 0) cross++; CHECK(cross == 1, "12 dB crossing rows: %d", cross); }
    // 5. noise alone and an unmodulated carrier: no tone
    { ToneScene sc(48000, 1.0, 3); auto t = runChunks(x40::Sinad, 48000, 10.0, std::ref(sc), 0, 146.52e6, 1.0); CHECK(t.rows.empty(), "noise: %zu rows", t.rows.size()); }
    { FmSrc f{48000, 0, 1000, 500, 0, noiseFor(30, 16000, 48000)}; auto t = runChunks(x40::Sinad, 48000, 10.0, std::ref(f), 0, 146.52e6, 1.0); CHECK(t.rows.empty(), "unmodulated carrier: %zu rows", t.rows.size()); }
    printf(fails ? "x40 sinad: %d FAILED\n" : "x40 sinad: ok\n", fails);
    return fails ? 1 : 0;
}
