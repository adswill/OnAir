// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 3.9 sensitivity pass D (docs/SENSITIVITY-3.9-D.md): the 90 % threshold of the HF keyboard decoders (Olivia, Contestia, MFSK, THOR,
// DominoEX) on their own transmitter's audio (8 kHz, hfdig_kbd.h) with white noise; SNR = signal power over the noise power in 2500 Hz,
// as in test_hfdig_kbd.cpp. A trial passes when the key text appears; each trial has its own noise and its own start in the text.
// Default (a few seconds of CPU): three trials at the recorded threshold + 2 dB must all pass, with a tuning error, and noise alone must give
// no characters. ONAIR_TEST_LONG=1: the sweep (10 trials a point, 1 dB steps) for each variant, plain and with a tuning error and a 100 ppm
// clock error; it prints the thresholds and the decoder's CPU per second of audio. ONAIR_KBDSENS_CASE=<n> runs one case,
// ONAIR_KBDSENS_HI=<dB> starts the sweep there.
#include "dect2/hfdig/hfdig_gen.h"
#include "dect2/hfdig/hfdig_kbd.h"
#include "dect2/hfdig/hfdig_tel.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <ctime>
using namespace dect2;
#include "../common/test_util.h"

namespace {

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    double u() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return ((s >> 11) + 0.5) / 9007199254740992.0; }
    double g() { return std::sqrt(-2 * std::log(u())) * std::cos(6.283185307179586 * u()); }
};

double cpuSeconds() { return (double)std::clock() / CLOCKS_PER_SEC; }   // process CPU time (wall time on Windows)

struct Case {
    const char* name;
    int fam, var, tones, bw;
    double secs;       // audio per trial
    double offHz;      // the tuning error of the offset scenario (inside the decoder's margin, no search)
    double thr;        // recorded 90 % threshold, dB in 2500 Hz (after this pass)
};

const Case kCases[] = {
    {"Olivia 8/250", KbdOlivia, -1, 8, 250, 60, 10, -15},
    {"Olivia 32/1000", KbdOlivia, -1, 32, 1000, 50, 20, -14},
    {"Contestia 16/500", KbdContestia, -1, 16, 500, 30, 25, -12},
    {"MFSK16", KbdMfsk, 3, 0, 0, 40, 6, -15},     // 3.9: -9 -> -15 dB (likelihood-ratio soft bits)
    {"THOR16", KbdThor, 4, 0, 0, 40, 30, -11},    // 3.9: about -5 -> -11 dB (soft bits instead of a gated hard decision)
    {"DominoEX11", KbdDominoex, 3, 0, 0, 40, 30, -12},  // 3.9: -7 -> -12 dB (squelch at a peak ratio of 4, was 6)
};

const char* kKey = "TEST 73";

double sigPower(const KbdTxConfig& c) {
    auto tx = makeKbdTx(c);
    std::vector<float> b(8000);
    double p = 0; size_t n = 0;
    for (int k = 0; k < 25; k++) { tx->generate(b.data(), b.size()); if (k < 5) continue; for (float v : b) { p += (double)v * v; n++; } }
    return p / (double)n;
}

KbdTxConfig txFor(const Case& c) {
    KbdTxConfig t;
    t.cfg.family = c.fam; t.cfg.variant = c.var; t.cfg.tones = c.tones; t.cfg.bandwidth = c.bw;
    t.text = "LY0AIR TEST 73 DE LY0AIR TEST 73 ";
    return t;
}

// one trial; returns whether the key was seen, and the decoder CPU seconds
bool trial(const Case& c, double snrDb, double offHz, double ppm, uint64_t seed, double sp, double* cpu = nullptr, uint64_t* chars = nullptr, bool noiseOnly = false) {
    KbdTxConfig tx = txFor(c);
    tx.offsetHz = offHz; tx.clockPpm = ppm;
    tx.leadSec = 0.3 + 5 * Rng(seed ^ 0x77).u();   // a different start in the text and the symbol grid each trial
    auto gen = makeKbdTx(tx);
    auto dec = makeKbdDecoder();
    KbdConfig k = tx.cfg; k.autoCentre = false;
    dec->setConfig(k);
    const double sigma = std::sqrt(sp / std::pow(10.0, snrDb / 10.0) * 4000.0 / 2500.0);
    Rng nz(seed);
    std::vector<float> buf(800);
    HfdigKbdTelemetry t;
    double used = 0;
    for (int blk = 0; blk < (int)(c.secs * 10); blk++) {
        gen->generate(buf.data(), buf.size());
        for (float& v : buf) v = (noiseOnly ? 0.f : v) + (float)(sigma * nz.g());
        const double c0 = cpuSeconds();
        dec->feedAudio(buf.data(), buf.size());
        used += cpuSeconds() - c0;
        if (!noiseOnly && blk % 10 == 9) { dec->telemetry(t); if (t.text.find(kKey) != std::string::npos) break; }
    }
    dec->telemetry(t);
    if (cpu) *cpu = used;
    if (chars) *chars = t.chars;
    return t.text.find(kKey) != std::string::npos;
}

int rateAt(const Case& c, double snr, double off, double ppm, int n, double sp, uint64_t salt) {
    int ok = 0;
    for (int i = 0; i < n; i++) ok += trial(c, snr, off, ppm, salt * 1000003ull + (uint64_t)(snr * 10 + 500) * 97 + (uint64_t)i, sp);
    return ok;
}

} // namespace

int main() {
    const bool longRun = getenv("ONAIR_TEST_LONG") && atoi(getenv("ONAIR_TEST_LONG")) > 0;
    const int only = getenv("ONAIR_KBDSENS_CASE") ? atoi(getenv("ONAIR_KBDSENS_CASE")) : -1;
    int ci = -1;
    for (const Case& c : kCases) {
        ci++;
        if (only >= 0 && ci != only) continue;
        const double sp = sigPower(txFor(c));
        double cpu = 0;
        CHECK(trial(c, 20, 0, 0, 1, sp, &cpu), "%s: no copy at 20 dB", c.name);
        printf("%s: CPU %.4f s per audio second (20 dB, until the key)\n", c.name, cpu / c.secs);
        if (!longRun) {
            CHECK(rateAt(c, c.thr + 2, 0, 0, 3, sp, 1) == 3, "%s at %.0f dB", c.name, c.thr + 2);
            CHECK(rateAt(c, c.thr + 2, c.offHz, 0, 3, sp, 2) == 3, "%s at %.0f dB, %+.0f Hz", c.name, c.thr + 2, c.offHz);
            uint64_t ch = 0;
            trial(c, 0, 0, 0, 99, sp, nullptr, &ch, true);
            CHECK(ch == 0, "%s: noise alone gave %llu characters", c.name, (unsigned long long)ch);
            continue;
        }
        struct Sc { const char* n; double off, ppm; };
        for (const Sc s : {Sc{"plain", 0, 0}, Sc{"offset", c.offHz, 0}, Sc{"clock 100 ppm", 0, 100}}) {
            double thr = NAN;
            std::string pts;
            const double hi = getenv("ONAIR_KBDSENS_HI") ? atof(getenv("ONAIR_KBDSENS_HI")) : c.thr + 4;
            for (double snr = hi; snr >= c.thr - 6; snr -= 1) {
                const int ok = rateAt(c, snr, s.off, s.ppm, 10, sp, 3 + (uint64_t)(s.off * 7 + s.ppm));
                char b[32]; snprintf(b, sizeof b, " %g:%d/10", snr, ok); pts += b;
                if (ok >= 9) thr = snr; else if (!std::isnan(thr) || snr < c.thr - 2) break;
            }
            printf("  %-16s %-14s threshold %s dB |%s\n", c.name, s.n, std::isnan(thr) ? "-" : std::to_string((int)thr).c_str(), pts.c_str());
            fflush(stdout);
            if (s.off == 0 && s.ppm == 0) CHECK(!std::isnan(thr) && thr <= c.thr + 1, "%s threshold", c.name);
        }
    }
    return testSummary("hfdig_kbdsens39");
}
