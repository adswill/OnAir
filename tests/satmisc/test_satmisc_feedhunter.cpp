// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Feed hunter state machine with an invented carrier list (no RF): every carrier is tried, new ones are flagged, wide ones skipped.
#include "dect2/satmisc/feed_hunter.h"
#include <cstdio>
using namespace dect2::satmisc;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
int main() {
    FeedHunter h;
    std::vector<FeedCarrierIn> c = {{1e6, 3e6, 12}, {6e6, 5e6, 10}, {-4e6, 27.5e6, 20}};
    double t = 0;
    int tried = 0, locked = 0;
    for (; t < 300; t += 1) {
        // carrier 2 appears at t = 60
        std::vector<FeedCarrierIn> now = c;
        if (t >= 60) now.push_back({-2e6, 2e6, 9});
        bool lock = h.hasRequest() && h.requestOffsetHz() == 1e6;
        h.update(t, now, 11e9, lock, lock ? "FEED ONE" : "");
    }
    for (const auto& f : h.feeds()) {
        printf("%.3f MHz %.1f Msym state %d new %d service %s\n", f.mhz, f.rateHz / 1e6, (int)f.state, f.isNew, f.service.c_str());
        if (f.state == FeedState::Locked) locked++;
        if (f.state != FeedState::Untried) tried++;
    }
    CHECK(h.feeds().size() == 3, "%zu feeds", h.feeds().size());   // the 27.5 Msym carrier is a programme, not a feed
    CHECK(tried == 3, "tried %d", tried);
    CHECK(locked == 1, "locked %d", locked);
    bool foundNew = false;
    for (const auto& f : h.feeds()) { if (f.rateHz == 2e6) foundNew = f.isNew; else CHECK(!f.isNew, "baseline flagged new"); }
    CHECK(foundNew, "the late carrier is not flagged new");
    CHECK(h.finishedRound(), "not finished");
    h.retry();
    int unt = 0; for (const auto& f : h.feeds()) if (f.state == FeedState::Untried) unt++;
    CHECK(unt == 2, "retry left %d", unt);
    // same carrier drifting a little does not make a new feed; a carrier leaving is not new again
    FeedHunter g;
    for (int i = 0; i < 10; i++) g.update(i, {{1e6 + i * 500, 3e6, 12}}, 0, false, "");
    CHECK(g.feeds().size() == 1, "drift made %zu", g.feeds().size());
    printf(fails ? "FAILED %d\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
