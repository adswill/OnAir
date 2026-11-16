// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Analog TV receiver after the engine threw a backlog away (Engine::catchUp, a slow computer with a live radio, #520): the receiver is fed
// signal with big gaps cut out of it (0.75 s at 10 Msps, again and again), in odd block sizes, with a carrier that jumps, and must neither
// crash nor lose its picture for good. Meant to run under AddressSanitizer as well.
#include "dect2/atv/atv_testkit.h"
#include <cmath>
#include <cstdio>
#include <random>
#include "../../core/src/atv/atv_video.h"

using namespace dect2;
using namespace dect2::atvkit;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

// modulation 0: AM (broadcast)
static void scenario(int modulation, const std::vector<cf32>& x, double rate) {
    std::mt19937 rng(520 + modulation);
    AtvReceiver rx;
    rx.setSilent(true);
    rx.setModulation(modulation);
    rx.configure(rate);
    uint64_t fseq = 0, frames = 0, seq = 0;
    AtvTelemetry t;
    size_t pos = 0;
    for (int round = 0; round < 16; round++) {
        const size_t stretch = (size_t)(rate * (0.05 + 0.4 * (rng() % 100) / 100.0));
        for (size_t done = 0; done < stretch;) {
            const size_t want = (rng() % 8 == 0) ? (size_t)(rng() % 3000000 + 1) : (size_t)(rng() % 70000 + 1);
            const size_t m = std::min({want, stretch - done, x.size() - pos});
            if (m == 0) { pos = 0; continue; }
            rx.feed(x.data() + pos, m);
            pos += m; done += m;
            if (rx.telemetry(t, seq)) seq = t.seq;
            while (auto f = rx.frame(fseq)) { (void)f; frames++; }
        }
        const size_t gap = (round % 3 == 0) ? (size_t)(0.75 * rate) : (size_t)(rng() % 7500000);
        pos = (pos + gap) % x.size();
    }
    printf("atv gap, modulation %d: %llu pictures, state %d\n", modulation, (unsigned long long)frames, t.state);
    CHECK(frames > 0, "modulation %d: no picture came out of the signal with gaps", modulation);
}

// The picture decoder with its sample counter just below 2^31, which 10 Msps reaches after 215 s: Windows' 32-bit long could not hold the
// positions then, and the line pixels were read from the wrong place, far outside the copied samples, until the program crashed (#520).
// The same video must give the same pictures wherever the counter starts.
static uint64_t pictures(uint64_t startCount, const std::vector<float>& comp, double rate) {
    AtvVideo v;
    v.configure(rate, true);
    v.setSampleCount(startCount);
    uint64_t n = 0;
    v.frameReady = [&](std::shared_ptr<const AtvFrame>) { n++; };
    std::vector<float> in(comp.size()), z(comp.size(), 0.f);
    for (size_t k = 0; k < comp.size(); k++) in[k] = -comp[k];   // the sync tip is the maximum at the decoder's input
    for (size_t i = 0; i < in.size(); i += 4000) {
        const size_t m = std::min<size_t>(4000, in.size() - i);
        v.process(in.data() + i, in.data() + i, z.data() + i, m);
    }
    return n;
}

int main() {
    AtvGenConfig c; c.sys = kAtvG; c.colour = kAtvPal; c.cnrDb = 30; c.rate = 10e6; c.sound = 4;
    const size_t secs = 6;
    std::vector<cf32> x((size_t)(secs * c.rate));
    { AtvGenerator g(c); g.generate(x.data(), x.size()); }
    for (auto& v : x) v *= 0.3f;
    {
        AtvGenConfig g2 = c; g2.rate = AtvGenerator::kInternalRate;
        AtvGenerator g(g2);
        std::vector<float> comp;
        g.setCompositeTap([&](const float* v, size_t n) { comp.insert(comp.end(), v, v + n); });
        std::vector<cf32> sink((size_t)(2.0 * g2.rate));
        g.generate(sink.data(), sink.size());
        const uint64_t a = pictures(0, comp, g2.rate), b = pictures((1ull << 31) - 5000000, comp, g2.rate), d = pictures(1ull << 32, comp, g2.rate);
        printf("atv counter: %llu pictures from 0, %llu across 2^31, %llu above 2^32\n", (unsigned long long)a, (unsigned long long)b, (unsigned long long)d);
        CHECK(a > 20, "only %llu pictures in 2 s of video", (unsigned long long)a);
        CHECK(b == a && d == a, "the sample counter changes the pictures: %llu %llu %llu", (unsigned long long)a, (unsigned long long)b, (unsigned long long)d);
    }
    scenario(0, x, c.rate);
    printf(fails ? "atv gap: FAILED (%d)\n" : "atv gap: ok\n", fails);
    return fails ? 1 : 0;
}
