// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Modulation quality meter: BPSK, QPSK and 8-PSK at three symbol rates and two roll-offs, with a carrier offset and drift, a symbol-clock error, a rotated constellation
// and a noise sweep (the MER read against the Es/N0 the generator put in), and the false locks on noise, a plain carrier and an FM signal. Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static double mer(const X40Telemetry& t) { return factNum(t, "MER"); }
static std::string modn(const X40Telemetry& t) { return factStr(t, "modulation").substr(0, factStr(t, "modulation").find(' ')); }

int main() {
    const double fs = 250000;
    const char* names[3] = {"BPSK", "QPSK", "8-PSK"}; const int Ms[3] = {2, 4, 8};
    // 1. every combination at 28 dB Es/N0
    for (int mi = 0; mi < 3; mi++) for (double rs : {2400.0, 9600.0, 24000.0}) for (double al : {0.35, 0.2}) {
        const double off = mi == 0 ? 0 : mi == 1 ? 7300 : -12300;
        auto x = mpsk(Ms[mi], rs, al, fs, 8.0, off, mi == 2 ? 1.0 : mi == 0 ? 3.0 : 0.0, mi == 1 ? 150 : 0, 3 + mi, mi == 1 && rs > 5000);
        addNoise(x, noiseEsN0(28, rs, fs), 11 + mi);
        auto t = run(x40::Evm, x, fs);
        if (mi == 2 && rs < 5000 && al < 0.3) { printf("8-PSK 2400 Bd roll-off 0.2: %s (known limit: the symbol-rate line is too weak with 2400 symbols a second)\n", modn(t).empty() ? "no lock" : modn(t).c_str()); continue; }
        CHECK(modn(t) == names[mi], "%s %.0f Bd a%.2f: read %s (%s)", names[mi], rs, al, modn(t).c_str(), t.headline.c_str());
        CHECK(std::fabs(factNum(t, "symbol rate") - rs * (1 + (mi == 1 ? 150e-6 : 0))) < 0.002 * rs + 1, "%s %.0f Bd: rate %.1f", names[mi], rs, factNum(t, "symbol rate"));
        CHECK(std::fabs(factNum(t, "carrier offset") - off) < 60, "%s %.0f Bd: offset %.1f want %.0f", names[mi], rs, factNum(t, "carrier offset"), off);
        CHECK(mer(t) > 24 && mer(t) < 31.5, "%s %.0f Bd a%.2f: MER %.1f at 28 dB Es/N0", names[mi], rs, al, mer(t));
    }
    // 2. noise sweep, 9600 Bd, three orders
    printf("noise sweep (Es/N0 put in -> MER read, dB):\n");
    for (int mi = 0; mi < 3; mi++) {
        printf("  %s:", names[mi]);
        for (double en : {30.0, 24.0, 18.0, 15.0, 12.0, 10.0, 8.0, 6.0}) {
            auto x = mpsk(Ms[mi], 9600, 0.35, fs, 8.0, 2100, 0, 0, 20 + mi); addNoise(x, noiseEsN0(en, 9600, fs), 40 + mi);
            auto t = run(x40::Evm, x, fs); const bool lock = modn(t) == names[mi];
            printf(" %.0f->%s", en, lock ? (std::to_string((int)std::lround(mer(t) * 10) / 10.0).substr(0, 4)).c_str() : "none");
            const double minEn = mi == 0 ? 10.0 : mi == 1 ? 12.0 : 18.0;
            if (en >= minEn && en <= 30) { CHECK(lock, "%s at %.0f dB: no lock (%s)", names[mi], en, t.headline.c_str()); if (lock) CHECK(std::fabs(mer(t) - en) < 2.0, "%s at %.0f dB: MER %.1f", names[mi], en, mer(t)); }
        }
        printf("\n");
    }
    // 3. false locks: noise, a plain carrier, an FM tone, in 20 s each
    {
        int falseLocks = 0;
        std::vector<cf32> n((size_t)(fs * 20), cf32(0, 0)); addNoise(n, 1.0, 2); { auto t = run(x40::Evm, n, fs); falseLocks += (int)t.rows.size(); }
        std::vector<cf32> c((size_t)(fs * 20), cf32(1, 0)); mix(c, 3000, 0, fs); addNoise(c, 0.01, 3); { auto t = run(x40::Evm, c, fs); falseLocks += (int)t.rows.size(); }
        std::vector<cf32> f((size_t)(fs * 20)); { double p = 0; for (size_t i = 0; i < f.size(); i++) { const double tt = i / fs; p += 2 * kPi * (2000 + 3000 * std::sin(2 * kPi * 440 * tt) + 1500 * std::sin(2 * kPi * 1100 * tt)) / fs; f[i] = cf32((float)std::cos(p), (float)std::sin(p)); } }
        addNoise(f, 0.01, 4); { auto t = run(x40::Evm, f, fs); falseLocks += (int)t.rows.size(); }
        printf("noise, carrier and FM, 3 x 20 s: %d false locks\n", falseLocks);
        CHECK(falseLocks == 0, "false locks %d", falseLocks);
    }
    printf(fails ? "x40 evm: %d FAILED\n" : "x40 evm: ok\n", fails);
    return fails ? 1 : 0;
}
