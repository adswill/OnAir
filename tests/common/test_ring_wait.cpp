// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// IqRingCpp::waitData(): the analysis thread sleeps until the radio has written enough (core/include/dect2/common/ring.h).
//  - returns at once when enough is there; times out (false) on an empty ring; true with less than asked when the time ran out
//  - the producer's write wakes it well before the timeout
//  - stress: a producer and a waiting consumer move every sample in order, with no wait stuck until its long timeout
#include "dect2/common/ring.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

using namespace std::chrono;
using dect2::cf32;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    {
        auto r = std::make_unique<dect2::IqRingCpp>(1u << 12);
        const auto t0 = steady_clock::now();
        CHECK(!r->waitData(1, milliseconds(20)), "an empty ring times out");
        const double ms = duration<double, std::milli>(steady_clock::now() - t0).count();
        CHECK(ms >= 15 && ms < 500, "the timeout holds (%.1f ms)", ms);
        std::vector<cf32> x(100);
        r->write(x.data(), x.size());
        CHECK(r->waitData(50, milliseconds(0)), "enough there: at once");
        CHECK(r->waitData(500, milliseconds(5)), "less than asked when the time ran out: still true");
    }
    {   // 4.0: samples the source never got (noteLost) are dropped, and apart from overflow (lostAtSource); resetDropped clears both
        auto r = std::make_unique<dect2::IqRingCpp>(1u << 10);
        std::vector<cf32> x(1500);
        r->write(x.data(), x.size());   // 476 do not fit: overflow
        r->noteLost(1000);              // a network radio's reconnect
        CHECK(r->dropped() == 1476 && r->lostAtSource() == 1000, "overflow %llu, source %llu", (unsigned long long)r->dropped(), (unsigned long long)r->lostAtSource());
        r->resetDropped();
        CHECK(r->dropped() == 0 && r->lostAtSource() == 0, "reset");
    }
    {   // woken by the write
        auto r = std::make_unique<dect2::IqRingCpp>(1u << 12);
        std::thread p([&] { std::this_thread::sleep_for(milliseconds(20)); std::vector<cf32> x(64); r->write(x.data(), x.size()); });
        const auto t0 = steady_clock::now();
        const bool ok = r->waitData(64, seconds(5));
        const double ms = duration<double, std::milli>(steady_clock::now() - t0).count();
        p.join();
        CHECK(ok && ms < 1000, "the write wakes the consumer (%.1f ms)", ms);
    }
    {   // stress: in order, nothing lost, no wait that only the timeout ended
        auto r = std::make_unique<dect2::IqRingCpp>(1u << 14);
        const size_t total = 4000000;
        std::atomic<bool> done{false};
        std::thread p([&] {
            std::vector<cf32> x(997);
            size_t v = 0;
            while (v < total) {
                const size_t n = std::min(x.size(), total - v);
                for (size_t i = 0; i < n; i++) x[i] = cf32((float)(v + i), 0.f);
                size_t w = 0;
                while (w < n) { w += r->write(x.data() + w, n - w); if (w < n) std::this_thread::yield(); }   // a full ring: try again
                v += n;
            }
            done = true;
        });
        std::vector<cf32> b(4096);
        size_t got = 0, bad = 0, longWaits = 0;
        while (got < total) {
            const size_t m = r->read(b.data(), b.size());
            for (size_t i = 0; i < m; i++) if (b[i].real() != (float)(got + i)) bad++;
            got += m;
            if (!m) {
                const auto t0 = steady_clock::now();
                r->waitData(std::min<size_t>(500, total - got), seconds(2));
                if (!done && steady_clock::now() - t0 > milliseconds(1500)) longWaits++;
            }
        }
        p.join();
        CHECK(bad == 0, "samples in order (%zu wrong)", bad);
        CHECK(longWaits == 0, "no lost wake-up (%zu waits ran into the timeout)", longWaits);
    }
    std::printf(failures ? "ring wait: %d failure(s)\n" : "ring wait: ok\n", failures);
    return failures ? 1 : 0;
}
