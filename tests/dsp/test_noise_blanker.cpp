// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The impulse noise blanker on synthetic signals: a tone with short strong clicks loses the clicks and keeps the tone; noise alone and
// a strong steady carrier are left alone; the width and the threshold do what they say.
#include "dect2/dsp/noise_blanker.h"
#include <cstdio>
#include <random>
#include <vector>

using dect2::cf32;
using dect2::NoiseBlanker;
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }

int main() {
    const double rate = 2.4e6;
    std::mt19937 rng(7);
    std::normal_distribution<float> g(0.f, 0.02f);
    const size_t N = 240000;   // 100 ms
    std::vector<cf32> sig(N), clean(N);
    for (size_t i = 0; i < N; i++) {
        const double ph = 2 * 3.14159265358979 * 25e3 * (double)i / rate;
        clean[i] = cf32(0.1f * (float)std::cos(ph), 0.1f * (float)std::sin(ph)) + cf32(g(rng), g(rng));
        sig[i] = clean[i];
    }
    std::vector<size_t> clicks;
    for (size_t at = 20000; at + 10 < N; at += 24000) {   // a click every 10 ms, 4 samples (under 2 us) at 30x the tone
        clicks.push_back(at);
        for (int k = 0; k < 4; k++) sig[at + k] = cf32(3.f, -2.f);
    }
    {   // on: every click gone, the tone kept outside the blanks
        std::vector<cf32> x = sig;
        NoiseBlanker nb; nb.set(8.f, 20e-6, rate);
        check(nb.widthSamples() == 48, "20 us at 2.4 Msps is 48 samples");
        for (size_t p = 0; p < N; p += 4096) nb.process(x.data() + p, std::min<size_t>(4096, N - p));   // in blocks, as the engine feeds it
        double clickEnergy = 0;
        for (size_t c : clicks) for (int k = 0; k < 4; k++) clickEnergy += std::norm(x[c + k]);
        check(clickEnergy == 0, "the clicks are zeroed");
        check(nb.events() == clicks.size(), "one blank per click");
        check(nb.blanked() == clicks.size() * (3 + 48), "each blank runs the width past the click's last sample");
        size_t same = 0, outside = 0;
        for (size_t i = 0; i < N; i++) {
            bool inBlank = false;
            for (size_t c : clicks) if (i >= c && i < c + 3 + 48) inBlank = true;
            if (inBlank) continue;
            outside++;
            if (x[i] == sig[i]) same++;
        }
        check(same == outside, "outside the blanks the samples are untouched");
        check(nb.mean() > 0.08 && nb.mean() < 0.14, "the average follows the tone, not the clicks");
    }
    {   // noise alone and a steady strong carrier: nothing blanked at the usual threshold
        std::vector<cf32> x = clean;
        NoiseBlanker nb; nb.set(8.f, 20e-6, rate);
        nb.process(x.data(), x.size());
        check(nb.blanked() == 0, "a clean tone in noise is never blanked");
        std::vector<cf32> y(N, cf32(0.7f, 0.1f));
        NoiseBlanker nb2; nb2.set(8.f, 20e-6, rate);
        nb2.process(y.data(), y.size());
        check(nb2.blanked() == 0, "a steady carrier is never blanked");
    }
    {   // a high threshold lets the clicks through; the limits clamp
        std::vector<cf32> x = sig;
        NoiseBlanker nb; nb.set(40.f, 20e-6, rate);
        nb.process(x.data(), x.size());
        check(nb.blanked() == 0, "clicks under the threshold pass");
        NoiseBlanker c; c.set(1000.f, 1.0, rate);
        check(c.widthSamples() == (int64_t)std::llround(NoiseBlanker::kMaxWidthSec * rate), "width clamps to 2 ms");
        c.set(0.f, 0.0, rate);
        check(c.widthSamples() == (int64_t)std::llround(NoiseBlanker::kMinWidthSec * rate), "width clamps to 2 us");
    }
    {   // a click right at the start of a block and one inside a running blank: the blank goes on across blocks and stretches
        std::vector<cf32> x(10000, cf32(0.05f, 0));
        x[5000] = cf32(5, 0); x[5030] = cf32(5, 0);
        NoiseBlanker nb; nb.set(8.f, 20e-6, rate);
        nb.process(x.data(), 5010);
        nb.process(x.data() + 5010, x.size() - 5010);
        check(nb.events() == 1 && nb.blanked() == 30 + 48, "a second click inside a blank stretches it");
        check(x[5000 + 77] == cf32(0, 0) && x[5000 + 78] == cf32(0.05f, 0), "the blank ends width samples after the last click");
    }
    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("noise blanker: all passed\n");
    return 0;
}
