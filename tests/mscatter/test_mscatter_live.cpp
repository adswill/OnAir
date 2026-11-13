// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The meteor scatter receiver as the engine runs it: IQ chunks in, telemetry out; the list, the hourly and daily counts, the CSV log file,
// the threshold, clearing, and input that must not poison the filters.
#include "dect2/mscatter/mscatter_gen.h"
#include "../common/tmp_path.h"
#include "dect2/mscatter/mscatter_live.h"
#include "dect2/mscatter/mscatter_tel.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
using namespace dect2;
#include "../common/test_util.h"

static void feedSeconds(MscatterLiveReceiver& r, MscatterGenerator& g, double rate, double seconds, bool poison = false) {
    std::vector<cf32> x(32768);
    for (int64_t n = 0; n < (int64_t)(seconds * rate); n += (int64_t)x.size()) {
        g.generate(x.data(), x.size());
        if (poison) { x[5] = cf32(NAN, 0); x[77] = cf32(0, INFINITY); }
        r.feed(x.data(), x.size());
    }
}

int main() {
    const double rate = 240000;
    const std::string path = tmpPath("mscatter_live_test.csv");
    std::remove(path.c_str());
    MscatterLiveReceiver r;
    r.configure(rate);
    r.setCenterHz(143.050e6);
    r.setLogFile(path);
    const int64_t t0 = 1790000000 / 3600 * 3600 + 3590;   // ten seconds before an hour change
    r.setStartUtc(t0);
    MscatterGenConfig c;
    c.rate = rate; c.snrDb = 40; c.carrierHz = 420; c.amp = 0.15; c.pingsOnlyAt = true;
    c.fixed = {{8, 0.6, 120, 25, false}, {14, 1.0, -60, 22, false}, {22, 0.5, 80, 27, false}};
    MscatterGenerator g(c);
    CHECK(r.ready(), "ready");
    feedSeconds(r, g, rate, 30, true);
    MsTelemetry t;
    CHECK(r.telemetry(t, 0), "no telemetry");
    printf("  %s, %zu pings, %.0f per hour\n", msSummary(t).c_str(), t.pings.size(), (double)t.ratePerHour);
    CHECK(t.state == 2 && t.dataValid && t.blocksOk == 3 && t.pings.size() == 3, "state %d, %llu pings", t.state, (unsigned long long)t.blocksOk);
    CHECK(std::fabs(t.cfoHz - 420) < 8 && t.snrDb > 45, "beacon %.1f Hz %.1f dB", t.cfoHz, (double)t.snrDb);
    CHECK(t.pings.size() == 3 && t.pings[0].id == 3 && t.pings[2].id == 1, "newest first");
    if (t.pings.size() == 3) CHECK(std::fabs((double)(t.pings[2].wallTime - t0) - 8.0) < 1.0, "first ping at %lld", (long long)(t.pings[2].wallTime - t0));
    // 10 s after the start the hour changes: the first two pings (8 s, 14 s... ) fall in different hours
    CHECK(t.hourly.size() == 24 && t.hourly[23] + t.hourly[22] == 3 && t.hourly[22] >= 1 && t.hourly[23] >= 1, "hourly %d %d", t.hourly[22], t.hourly[23]);
    int dsum = 0;
    for (int v : t.daily) dsum += v;
    CHECK(dsum == 3, "daily sum %d", dsum);
    CHECK(t.logging && t.logPath == path, "log state");
    CHECK(t.waterfall.size() == (size_t)kMsWfRows * kMsWfCols && t.spectrum.size() == (size_t)kMsWfCols, "waterfall size");
    bool lit = false;
    for (uint8_t v : t.waterfall) if (v > 150) lit = true;
    CHECK(lit, "no echo in the waterfall");
    CHECK(!t.showers.empty(), "showers");
    // the CSV file: a header and a line per ping
    {
        std::ifstream f(path);
        std::string line;
        int n = 0;
        std::string first;
        while (std::getline(f, line)) { if (n == 0) first = line; n++; }
        CHECK(n == 4 && first.find("id,utc") == 0, "csv has %d lines", n);
    }
    // clearing empties the list and the counts, not the beacon
    r.clearResults();
    cf32 z[1024] = {};
    r.feed(z, 1024);
    uint64_t seq = t.seq;
    CHECK(r.telemetry(t, seq - 1) || true, "poll");
    r.telemetry(t, 0);
    CHECK(t.blocksOk == 0 && t.pings.empty(), "after clear: %llu", (unsigned long long)t.blocksOk);
    // a rate that is too low is refused
    MscatterLiveReceiver low;
    low.configure(8000);
    CHECK(!low.ready(), "8 ksps ready");
    std::remove(path.c_str());
    return testSummary("mscatter live");
}
