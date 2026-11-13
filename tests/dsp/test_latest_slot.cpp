// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LatestSlot (core/include/dect2/common/latest_slot.h): the tuning mailbox. A burst of requests from the UI while the worker is busy with a
// slow radio call: the worker ends on the newest request, never runs the overtaken ones, and the counters add up.
#include "dect2/common/latest_slot.h"
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }

int main() {
    using namespace dect2;
    using namespace std::chrono;
    {   // single thread: post twice, take once: the second wins, one coalesced
        LatestSlot<int> s;
        check(!s.post(1), "first post replaces nothing");
        check(s.post(2), "second post replaces the first");
        int v = 0;
        check(s.tryTake(v) && v == 2, "the newest value is taken");
        s.done();
        check(!s.tryTake(v), "nothing left");
        check(s.posted() == 2 && s.taken() == 1 && s.coalesced() == 1, "counters");
        s.post(5); s.drop();
        check(!s.pending() && s.coalesced() == 2, "drop discards the pending one");
    }
    {   // a worker whose call takes 8 ms, 200 requests posted 1 ms apart (a fast wheel): it ends on the last, does far fewer calls
        LatestSlot<int> s;
        std::atomic<bool> stop{false};
        std::vector<int> ran;
        std::thread w([&] {
            int v;
            while (!stop) {
                if (!s.take(v, milliseconds(20))) continue;
                std::this_thread::sleep_for(milliseconds(8));   // the driver call
                ran.push_back(v);
                s.done();
            }
        });
        // 1 ms apart on the clock: sleep_for(1 ms) lasts a whole timer tick (about 15.6 ms) on Windows, as long as the 8 ms call, and nothing coalesced
        const auto t0 = steady_clock::now();
        for (int i = 1; i <= 200; i++) { s.post(i); while (steady_clock::now() < t0 + milliseconds(i)) std::this_thread::yield(); }
        check(s.waitIdle(seconds(2)), "the worker goes idle");
        stop = true; s.close(); w.join();
        check(!ran.empty() && ran.back() == 200, "the last request is the one the radio ends on");
        bool increasing = true;
        for (size_t i = 1; i < ran.size(); i++) increasing = increasing && ran[i] > ran[i - 1];
        check(increasing, "never an older request after a newer one");
        check(ran.size() < 60, "most requests were coalesced (one call per ~8 ms, not 200)");
        check(s.posted() == 200 && s.taken() == ran.size() && s.coalesced() == 200 - ran.size(), "posted = taken + coalesced");
        printf("latest_slot: 200 posted, %zu radio calls, %llu coalesced\n", ran.size(), (unsigned long long)s.coalesced());
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("latest_slot: all ok\n");
    return 0;
}
