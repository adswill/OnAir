// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Narrowband FM deviation and CTCSS meter: voice-like audio (a sum of tones with a few levels) plus a CTCSS tone through an FM modulator, with a carrier offset, noise, and over-deviation; the voice peak
// deviation, the CTCSS frequency (and the nearest standard tone), its deviation and the offset are compared with the generator. Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Tx {
    double fs, voiceDev, ctcss, ctcssDev, lo, drift, noise; Rng rng{5}; double ph = 0; double peak = 0;
    double audio(double t) const { return (0.45 * std::sin(2 * kPi * 523.0 * t) + 0.3 * std::sin(2 * kPi * 1117.0 * t + 1.0) + 0.2 * std::sin(2 * kPi * 1931.0 * t + 2.0) + 0.1 * std::sin(2 * kPi * 2707.0 * t + 0.3)) * voiceDev; }
    std::vector<cf32> operator()(double t0, size_t n) {
        std::vector<cf32> x(n); const float sg = (float)std::sqrt(noise / 2);
        for (size_t i = 0; i < n; i++) { const double t = t0 + i / fs; const double dev = audio(t) + ctcssDev * std::sin(2 * kPi * ctcss * t + 0.5); ph += 2 * kPi * (lo + drift * t + dev) / fs; x[i] = cf32((float)std::cos(ph), (float)std::sin(ph)) + cf32(sg * rng.g(), sg * rng.g()); }
        return x;
    }
};
static double voicePeakOf(const Tx& tx) { double pk = 0; for (double t = 0; t < 20; t += 1e-4) pk = std::max(pk, std::fabs(tx.audio(t))); return pk; }

int main() {
    // 1. voice 2.0 kHz peak, CTCSS 100.0 / 123.0 / 67.0 / 254.1 Hz with 400 / 350 / 500 / 600 Hz deviation, offsets, three input rates
    const struct { double fs, tone, dev, off; } cs[] = {{48000, 100.0, 400, 1500}, {96000, 123.0, 350, -800}, {250000, 67.0, 500, 2500}, {48000, 254.1, 600, -2000}, {48000, 88.5, 380, 0}};
    for (auto& c : cs) {
        Tx tx{c.fs, 1600, c.tone, c.dev, c.off, 3.0, noiseFor(30, 16000, c.fs)}; const double vp = voicePeakOf(tx);
        auto t = runChunks(x40::NfmDev, c.fs, 10.0, std::ref(tx), 0, 146.52e6, 1.0);
        const double ctc = factNum(t, "CTCSS tone"), dv = factNum(t, "CTCSS deviation"), vpr = factNum(t, "voice peak deviation") * 1000, off = factNum(t, "carrier offset");
        CHECK(std::fabs(vpr - vp) < 0.08 * vp, "fs %.0f: voice peak %.0f Hz want %.0f", c.fs, vpr, vp);
        CHECK(std::fabs(ctc - c.tone) < 0.4, "tone %.2f want %.1f", ctc, c.tone);
        CHECK(factStr(t, "CTCSS tone").find(std::to_string(c.tone).substr(0, std::to_string(c.tone).find(".") + 2)) != std::string::npos, "nearest %s", factStr(t, "CTCSS tone").c_str());
        CHECK(std::fabs(dv - c.dev) < 0.08 * c.dev, "tone deviation %.0f want %.0f", dv, c.dev);
        CHECK(std::fabs(off - (c.off + 15)) < 40, "offset %.0f want %.0f", off, c.off + 15.0);
    }
    // 2. no CTCSS; over-deviation (voice 3.5 kHz and 6.5 kHz peaks)
    { Tx tx{48000, 3500, 100, 0, 0, 0, noiseFor(30, 16000, 48000)}; auto t = runChunks(x40::NfmDev, 48000, 10.0, std::ref(tx), 0, 146.52e6, 1.0); CHECK(factStr(t, "CTCSS tone").rfind("none", 0) == 0, "tone: %s", factStr(t, "CTCSS tone").c_str()); CHECK(countKind(t, "Over-deviation") >= 1, "over 2.5 kHz not logged"); }
    { Tx tx{48000, 6500, 100, 0, 0, 0, noiseFor(30, 16000, 48000)}; auto t = runChunks(x40::NfmDev, 48000, 10.0, std::ref(tx), 0, 146.52e6, 1.0); const std::string o = factStr(t, "over-deviation"); CHECK(o.find("over 5.0 kHz") != std::string::npos && atoi(strstr(o.c_str(), "kHz, ") + 5) >= 3, "over 5 kHz: %s", o.c_str()); }
    // 3. the CTCSS tone stops at 5 s: logged as gone; noise sweep: tone found against the carrier to noise
    { Tx tx{48000, 1500, 100.0, 400, 0, 0, noiseFor(30, 16000, 48000)}; auto gen = [&](double t0, size_t n) { tx.ctcssDev = t0 < 6 ? 400 : 0; return tx(t0, n); }; auto t = runChunks(x40::NfmDev, 48000, 14.0, gen, 0, 146.52e6, 1.0); bool gone = false; for (auto& r : t.rows) if (r.kind == "CTCSS" && r.id == "gone") gone = true; CHECK(gone, "tone stop not logged"); }
    printf("noise sweep (carrier to noise in 16 kHz): voice peak read (true %.0f Hz), CTCSS read:\n", voicePeakOf(Tx{48000, 1600, 0, 0, 0, 0, 0}));
    for (double cnr : {30.0, 20.0, 15.0, 12.0, 10.0}) { Tx tx{48000, 1600, 100.0, 400, 0, 0, noiseFor(cnr, 16000, 48000)}; auto t = runChunks(x40::NfmDev, 48000, 10.0, std::ref(tx), 0, 146.52e6, 1.0);
        printf("  %.0f dB: voice %.0f Hz, %s\n", cnr, factNum(t, "voice peak deviation") * 1000, factStr(t, "CTCSS tone").substr(0, 40).c_str()); if (cnr >= 15) CHECK(std::fabs(factNum(t, "CTCSS tone") - 100.0) < 0.5, "%.0f dB tone %.1f", cnr, factNum(t, "CTCSS tone")); }
    { ToneScene sc(48000, 1.0, 3); auto t = runChunks(x40::NfmDev, 48000, 10.0, std::ref(sc), 0, 146.52e6, 1.0); CHECK(factStr(t, "CTCSS tone").rfind("none", 0) == 0, "noise: %s", factStr(t, "CTCSS tone").c_str()); }
    printf(fails ? "x40 nfmdev: %d FAILED\n" : "x40 nfmdev: ok\n", fails);
    return fails ? 1 : 0;
}
