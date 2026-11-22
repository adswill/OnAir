// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Audio equaliser: flat is transparent, shelves and bands give the set gain where they should and little elsewhere, presets are sane,
// extreme settings and low sample rates stay stable, and the stage works inside the cleanup chain.
#include "dect2/audio/audio_cleanup.h"
#include "dect2/audio/audio_eq.h"
#include <cmath>
#include <cstdio>
#include <vector>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
using namespace dect2;

// gain in dB of a sine through the equaliser, measured after the start-up
static double measure(const ToneParams& p, double hz, float rate = 48000) {
    ToneEq e; e.configure(p, rate);
    const int n = (int)rate;
    double in = 0, out = 0;
    for (int i = 0; i < n; i++) {
        const float x = 0.3f * (float)std::sin(2 * 3.14159265358979 * hz * i / rate), y = e.tick(x);
        if (i > n / 2) { in += (double)x * x; out += (double)y * y; }
    }
    return 10 * std::log10(out / in);
}

int main() {
    // flat_is_transparent
    {
        ToneParams f; CHECK(f.flat());
        ToneEq e; e.configure(f, 48000);
        float m = 0; for (int i = 0; i < 4800; i++) { const float x = 0.5f * (float)std::sin(0.05 * i); m = std::max(m, std::fabs(e.tick(x) - x)); }
        CHECK(m < 1e-6f);
        CHECK(std::fabs(e.responseDb(1000)) < 1e-9);
    }
    // bass_and_treble
    {
        ToneParams p; p.bass = 12;
        CHECK(std::fabs(measure(p, 40) - 12) < 2.0);       // well under the corner: about the full gain
        CHECK(std::fabs(measure(p, 3000)) < 0.7);
        p = ToneParams(); p.treble = -12;
        CHECK(std::fabs(measure(p, 14000) + 12) < 2.0);
        CHECK(std::fabs(measure(p, 500)) < 0.7);
    }
    // graphic_bands
    {
        for (int b = 2; b < 7; b++) {   // 125 Hz .. 2 kHz: the probe at 6 x the band stays under the Nyquist rate
            ToneParams p; p.band[(size_t)b] = 9;
            const double hz = kToneBandHz[b];
            CHECK(std::fabs(measure(p, hz) - 9) < 0.7);
            CHECK(std::fabs(measure(p, hz / 8)) < 2.0);
            CHECK(std::fabs(measure(p, hz * 6)) < 2.0);
        }
        ToneParams c; c.band[5] = -12;
        CHECK(std::fabs(measure(c, 1000) + 12) < 0.7);
    }
    // gains_are_clamped
    {
        ToneParams p; p.bass = 40; p.treble = -40; p.band[3] = 99; p.band[4] = NAN;
        p.clamp();
        CHECK(p.bass == 12 && p.treble == -12 && p.band[3] == 12 && p.band[4] == 0);
    }
    // presets_sane
    {
        CHECK(toneEqPresetParams(ToneEqPreset::Flat).flat());
        for (int i = 0; i < (int)ToneEqPreset::Count; i++) {
            const ToneParams q = toneEqPresetParams((ToneEqPreset)i);
            CHECK(q.bass >= -12 && q.bass <= 12 && q.treble >= -12 && q.treble <= 12);
            for (float g : q.band) CHECK(g >= -12 && g <= 12);
            CHECK(toneEqPresetOf(q) == (ToneEqPreset)i);
            CHECK(toneEqPresetName((ToneEqPreset)i)[0] != 0);
        }
        CHECK(measure(toneEqPresetParams(ToneEqPreset::Voice), 50) < -6);       // voice: rumble down
        CHECK(measure(toneEqPresetParams(ToneEqPreset::Voice), 3000) > 2);      // and presence up
        CHECK(measure(toneEqPresetParams(ToneEqPreset::Hearing), 4000) > 4);
        ToneParams own; own.band[2] = 7.f;
        CHECK(toneEqPresetOf(own) == ToneEqPreset::Count);
    }
    // stable_at_low_rates_and_extremes
    {
        for (float rate : {8000.f, 11025.f, 16000.f, 44100.f, 192000.f}) {
            ToneParams p; p.bass = 12; p.treble = 12; for (float& g : p.band) g = 12;
            ToneEq e; e.configure(p, rate);
            bool ok = true; unsigned s = 1;
            for (int i = 0; i < 20000; i++) { s = s * 1664525u + 1013904223u; const float x = ((s >> 8) / 8388608.f - 1.f) * 0.5f; const float y = e.tick(x); if (!std::isfinite(y) || std::fabs(y) > 400.f) ok = false; }
            CHECK(ok);
        }
    }
    // inside_the_cleanup_chain: master + eq on, bass +12 lifts an 80 Hz tone; eq off leaves it alone
    {
        auto run = [](bool eqOn) {
            AudioCleanupConfig cfg; AudioCleanupSettings s; s.master = true; s.eq = eqOn; s.eqBass = 12;
            cfg.set(s);
            AudioCleanup c; c.setConfig(&cfg); c.prepare(48000);
            std::vector<float> in(2 * 1024), sc(2 * 1024);
            double e = 0; long cnt = 0; int pos = 0;
            for (int blk = 0; blk < 150; blk++) {
                for (int i = 0; i < 1024; i++) { const float x = 0.2f * (float)std::sin(2 * 3.14159265358979 * 80 * (pos++) / 48000.0); in[2 * i] = in[2 * i + 1] = x; }
                const float* o = c.process(in.data(), 1024, sc.data());
                if (blk > 100) for (int i = 0; i < 1024; i++) { e += (double)o[2 * i] * o[2 * i]; cnt++; }
            }
            return 10 * std::log10(e / cnt / (0.2 * 0.2 / 2));
        };
        CHECK(run(true) > 6);
        CHECK(std::fabs(run(false)) < 1.5);
        AudioCleanupSettings s; s.eq = true; s.eqBass = 40; s.eqTreble = -3; s.eqBand[4] = 5;
        AudioCleanupConfig cfg; cfg.set(s);
        const AudioCleanupSettings b = cfg.get();
        CHECK(b.eq && b.eqBass == 12 && b.eqTreble == -3 && b.eqBand[4] == 5);   // clamped, round trip
    }
    printf(fails ? "FAILED\n" : "all ok\n");
    return fails ? 1 : 0;
}
