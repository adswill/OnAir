// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The radiofax directory: every station has frequencies in the HF range, every broadcast names a known station at a valid time,
// the dial is 1.9 kHz below the carrier, and the "next broadcasts" list is in order and starts within a day.
#include "dect2/marine/marine_wefax_stations.h"
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace dect2;

#include "../common/test_util.h"

int main() {
    int nf = 0;
    for (const auto& s : wefaxStations()) {
        CHECK(!s.carrierKhz.empty(), "%s: no frequency", s.id);
        for (double k : s.carrierKhz) { CHECK(k >= 2000 && k <= 26000, "%s: %.1f kHz", s.id, k); nf++; }
        CHECK(wefaxStation(s.id) == &s, "%s: id not unique", s.id);
    }
    for (const auto& b : wefaxBroadcasts()) {
        CHECK(wefaxStation(b.station) != nullptr, "broadcast of unknown station %s", b.station);
        CHECK(b.utcMin >= 0 && b.utcMin < 1440 && (b.lpm == 60 || b.lpm == 90 || b.lpm == 120 || b.lpm == 240), "%s: time %d lpm %d", b.station, b.utcMin, b.lpm);
    }
    CHECK(std::fabs(wefaxDialKhz(7880) - 7878.1) < 1e-9 && std::fabs(wefaxDialKhz(8040) - 8038.1) < 1e-9, "dial");
    const long long now = 1760227200LL + 3 * 3600 + 17 * 60;   // 03:17 UTC
    const auto next = wefaxNext(now, 20);
    CHECK(next.size() == 20, "%zu next", next.size());
    for (size_t i = 0; i < next.size(); i++) {
        CHECK(next[i].startUtc >= now - 600 && next[i].startUtc < now + 86400, "next %zu at %lld", i, next[i].startUtc - now);
        if (i) CHECK(next[i].startUtc >= next[i - 1].startUtc, "order");
    }
    for (const auto& n : wefaxNext(now, 100, "Europe")) CHECK(strcmp(n.s->region, "Europe") == 0, "region filter");
    printf("%zu stations, %d frequencies, %zu broadcasts; next: %s %02d:%02d\n", wefaxStations().size(), nf, wefaxBroadcasts().size(),
           next.empty() ? "-" : next[0].s->callsign, next.empty() ? 0 : next[0].b->utcMin / 60, next.empty() ? 0 : next[0].b->utcMin % 60);
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
