// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AIS: receiving only noise must not produce stations or positions (issue #87).
#include "dect2/ais/ais_rx.h"
#include "dect2/common/stable_random.h"
#include "dect2/ais/ais_testutil.h"
#include "../common/test_long.h"
#include <cstdio>
#include <random>
using namespace dect2;

int main() {
    std::mt19937 rng(7);
    dect2::normal_distribution<float> g(0.f, 1.f);
    for (double fs : {3e6, 2e6, 1e6}) {
        AisReceiver rx;
        rx.configure(fs);
        std::vector<cf32> x((size_t)(fs * testLongPick(15, 60)));   // 15 s of noise per rate by default, 60 s in the long run
        const double gateHz = fs > 0 ? 50.0 : 0;  // keyed noise: bursts of strong noise, as a pulsed interferer
        for (size_t i = 0; i < x.size(); i++) {
            const double ph = std::fmod((double)i / fs * gateHz, 1.0);
            const float a = ph < 0.3 ? 1e-1f : 1e-3f;
            x[i] = cf32{g(rng) * a, g(rng) * a};
        }
        const AisTelemetry t = aistest::runReceiver(rx, x);
        int pos = 0;
        for (const auto& v : t.vessels) pos += v.hasPos;
        printf("fs %.0f: bursts %llu ok %llu bad %llu vessels %zu withpos %d\n", fs, (unsigned long long)t.bursts, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.vessels.size(), pos);
        if (!t.vessels.empty() || pos) { printf("FAIL: stations from noise\n"); return 1; }
    }
    printf("ok\n");
    return 0;
}
