// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HD Radio Whole span: the sideband-pair finder, two generated hybrid stations and a plain FM-like carrier in one capture, then the engine's bank. Simulated (hdr_gen).
#include "dect2/hdr/hdr_find.h"
#include "dect2/hdr/hdr_gen.h"
#include "dect2/hdr/hdr_rx.h"
#include "dect2/engine/wide_modes.h"
#include "dect2/engine/wide_bank.h"
#include "dect2/t2/t2rx.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const double rate = 4e6, centre = 97e6;
    const double secs = 12; const size_t chunk = 1 << 18; const uint64_t total = (uint64_t)(rate * secs);
    SynthConfig sc; sc.snrDb = 30;
    auto a = makeHdrSynth(sc, rate), b = makeHdrSynth(sc, rate);
    CHECK(a && b, "generator");
    std::vector<cf32> x(chunk), y(chunk), s(chunk);
    double noise = 0;   // white noise across the capture, 25 dB under the station's power in its 400 kHz
    { auto c = makeHdrSynth(sc, rate); c->generate(x.data(), chunk); double p = 0; for (auto& v : x) p += std::norm(v); p /= chunk; noise = std::sqrt(p * (400e3 / rate) / 2 * std::pow(10.0, -25 / 10.0) * (rate / 400e3)); }
    HdrFinder f; f.configure(rate, centre);
    WideModes w; w.setCenterHz(centre); w.configure(22, rate, 0);
    WideTuning tun; tun.cpuBudget = 0; w.setTuning(tun);
    std::mt19937 rng(4); std::normal_distribution<float> g(0, (float)noise);
    const double wa = 2 * M_PI * -600000.0 / rate, wb = 2 * M_PI * 700000.0 / rate, wc = 2 * M_PI * 100000.0 / rate;
    uint64_t n = 0; double ph = 0;
    while (n < total) {
        a->generate(x.data(), chunk); b->generate(y.data(), chunk);
        for (size_t i = 0; i < chunk; i++) {
            const double k = (double)(n + i);
            ph += wc + 0.8 * std::sin(k * 2 * M_PI * 1000 / rate) * 2 * M_PI * 75000.0 / rate;     // an analogue FM station with no digital sidebands
            s[i] = x[i] * cf32((float)std::cos(wa * k), (float)std::sin(wa * k)) + y[i] * cf32((float)std::cos(wb * k), (float)std::sin(wb * k)) + cf32(0.15f * (float)std::cos(ph), 0.15f * (float)std::sin(ph)) + cf32(g(rng), g(rng));
        }
        f.feed(s.data(), chunk); w.feed(22, s.data(), chunk); n += chunk;
    }
    auto h = f.hits();
    bool foundA = false, foundB = false; int extra = 0;
    for (auto& e : h) { printf("hit %+.0f kHz lower %.1f upper %.1f dB\n", e.hz / 1e3, e.lowerDb, e.upperDb); if (std::fabs(e.hz + 600000) < 1000) foundA = true; else if (std::fabs(e.hz - 700000) < 1000) foundB = true; else extra++; }
    CHECK(foundA && foundB, "both stations found");
    CHECK(extra == 0, "no false hit (analogue FM at +100 kHz): %d", extra);
    RxTelemetry t; uint64_t seq = 0; CHECK(w.telemetry(22, t, seq), "telemetry");
    int locked = 0; for (const auto& r : t.wide) { printf("carrier %+.0f kHz state %d %s\n", r.hz / 1e3, r.state, r.info.c_str()); if (r.state == 2) locked++; }
    CHECK(locked == 2, "two locked: %d", locked);
    return testSummary("hdr_wide");
}
