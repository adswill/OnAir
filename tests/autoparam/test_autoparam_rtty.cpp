// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Auto for RTTY (docs/AUTO-PARAMS-3.8.md): every speed, shift and polarity with real-world impairments, simulated signals only (makeRttyTxAudio). Offset,
// drift, a noise sweep, a second signal in view, noise alone, change of signal while running, a manual setting that must stick.
#include "dect2/hfdig/hfdig_gen.h"
#include "dect2/hfdig/hfdig_rtty.h"
#include "dect2/autoparam/autoparam.h"
#include "sim.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
using namespace dect2;
using namespace dect2::autotest;
#include "../common/test_util.h"

namespace {
std::vector<float> rtty(int b, int s, bool rev, double low, double seconds) {
    auto g = makeRttyTxAudio("", b, s, rev, low);
    std::vector<float> a((size_t)(seconds * 8000));
    g->generate(a.data(), a.size());
    return a;
}
bool nearShift(int a, int b) { return std::fabs(kRttyShiftTable[a] - kRttyShiftTable[b]) <= 30; }
bool hit(const std::vector<RttyAutoHyp>& h, int b, int s, bool rev) { return !h.empty() && h[0].baudIdx == b && nearShift(h[0].shiftIdx, s) && h[0].reverse == rev; }

// feeds in blocks and waits for any analysis job, so the outcome does not depend on the speed of this machine
void runWaitable(HfdigRtty& d, const std::vector<float>& a) {
    std::vector<float> x(800);
    for (size_t i = 0; i < a.size(); i += 800) {
        const size_t m = std::min<size_t>(800, a.size() - i);
        d.feedAudio(&a[i], m);
        if (i % 8000 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
}

int main() {
    const auto t0 = std::chrono::steady_clock::now();
    // 1. all 40 combinations with +-60 Hz offset and drift of 6 Hz/min at 12 dB in 3 kHz
    int ok = 0, n = 0;
    for (int b = 0; b < kRttyBauds; b++)
        for (int s = 0; s < kRttyShifts; s++)
            for (int rv = 0; rv < 2; rv++) {
                auto a = rtty(b, s, rv != 0, 900 + 37.0 * (b * 5 + s + rv * 2), 12);
                shift(a, 8000, (n % 2 ? 60 : -60), 6);
                addNoise(a, 8000, 12, 3000, (uint64_t)(n + 1));
                const auto h = rttyAutoAnalyse(a.data(), a.size());
                n++;
                if (hit(h, b, s, rv != 0)) ok++;
                else printf("  miss baud %g shift %g rev %d -> %s\n", kRttyBaudTable[b], kRttyShiftTable[s], rv, h.empty() ? "none" : (std::to_string(h[0].baud) + "/" + std::to_string(h[0].shiftHz) + (h[0].reverse ? " rev" : "")).c_str());
            }
    printf("RTTY all combinations, +-60 Hz offset, 6 Hz/min drift, 12 dB: %d of %d right\n", ok, n);
    CHECK(ok * 100 >= n * 90, "combinations %d of %d", ok, n);
    // 2. noise sweep on the two most used settings
    for (double snr : {20.0, 10.0, 5.0, 2.0, 0.0, -3.0}) {
        int g = 0, tot = 0;
        for (int c = 0; c < 2; c++)
            for (int k = 0; k < 5; k++) {
                const int b = c ? 2 : 0, s = c ? 2 : 0;
                auto a = rtty(b, s, c == 1, 1000 + 170.0 * k, 12);
                addNoise(a, 8000, snr, 3000, (uint64_t)(500 + k + 10 * c));
                tot++;
                g += hit(rttyAutoAnalyse(a.data(), a.size()), b, s, c == 1);
            }
        printf("noise %5.1f dB in 3 kHz: %d of %d right\n", snr, g, tot);
        if (snr >= 5) CHECK(g * 100 >= tot * 80, "noise %g: %d of %d", snr, g, tot);
    }
    // 3. two signals in view: the stronger is found, the other shows as an alternative or not at all
    {
        int g = 0, tot = 6;
        for (int k = 0; k < tot; k++) {
            auto a = rtty(0, 0, false, 1000 + 40.0 * k, 12);
            auto b = rtty(2, 2, false, 2200, 12);
            for (size_t i = 0; i < a.size(); i++) a[i] = a[i] + 0.4f * b[i];
            addNoise(a, 8000, 15, 3000, (uint64_t)(900 + k));
            g += hit(rttyAutoAnalyse(a.data(), a.size()), 0, 0, false);
        }
        printf("two signals in view, 8 dB apart: strongest found %d of %d\n", g, tot);
        CHECK(g >= tot - 1, "two signals %d of %d", g, tot);
    }
    // 4. noise alone must not lock, on the live path too
    {
        int bad = 0;
        const int tot = 40;
        for (int k = 0; k < tot; k++) {
            auto a = noiseOnly(8000, 10, 0.2, (uint64_t)(2000 + k));
            bad += !rttyAutoAnalyse(a.data(), a.size()).empty();
        }
        printf("noise alone: %d false locks of %d\n", bad, tot);
        CHECK(bad == 0, "noise false locks %d of %d", bad, tot);
        auto d = makeRttyDecoder();
        d->setAuto(true);
        auto a = noiseOnly(8000, 30, 0.2, 7);
        runWaitable(*d, a);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        d->feedAudio(a.data(), 800);
        HfdigRttyTelemetry t; d->telemetry(t);
        printf("noise alone, live Auto for 30 s: locked %d\n", (int)t.autoLocked);
        CHECK(!t.autoLocked, "live noise locked");
    }
    // 5. live: wrong starting values, then the signal changes, and Auto follows
    {
        auto d = makeRttyDecoder();
        d->setAuto(true);
        auto a = rtty(2, 3, true, 1900, 16);   // 75 / 450 / reversed
        addNoise(a, 8000, 14, 3000, 11);
        runWaitable(*d, a);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        d->feedAudio(a.data(), 800);
        HfdigRttyTelemetry t; d->telemetry(t);
        printf("live: first signal: baud %g shift %g rev %d, locked %d, %llu chars\n", t.baud, t.shiftHz, (int)t.reverse, (int)t.autoLocked, (unsigned long long)t.chars);
        CHECK(t.autoLocked && t.baud == 75 && std::fabs(t.shiftHz - 450) <= 30 && t.reverse, "live first");
        auto b = rtty(0, 0, false, 1200, 26);  // 45.45 / 170 / normal: the old signal is gone
        addNoise(b, 8000, 14, 3000, 12);
        runWaitable(*d, b);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        d->feedAudio(b.data(), 800);
        d->telemetry(t);
        printf("live: after the change: baud %g shift %g rev %d, locked %d, %llu chars\n", t.baud, t.shiftHz, (int)t.reverse, (int)t.autoLocked, (unsigned long long)t.chars);
        CHECK(t.autoLocked && t.baud == 45.45 && t.shiftHz <= 200 && !t.reverse, "live relock");
        // a manual setting stops Auto and stays
        d->setBaudIndex(1);
        CHECK(!d->autoOn(), "manual turns Auto off");
        auto c = rtty(2, 2, false, 1500, 16);
        addNoise(c, 8000, 14, 3000, 13);
        runWaitable(*d, c);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        d->feedAudio(c.data(), 800);
        d->telemetry(t);
        printf("manual after Auto: baud %g (must stay 50)\n", t.baud);
        CHECK(t.baud == 50 && d->baudIndex() == 1, "manual kept");
    }
    printf("%.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return testSummary("autoparam_rtty");
}
