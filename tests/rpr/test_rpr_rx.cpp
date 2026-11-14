// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Robust Packet receiver through the radio front end: the generator's signal as the upper sideband of a radio at 250 ksps (the 20 kHz tuning offset
// mixed out), with the radio tuned off by +-50 Hz, both speed levels, and noise alone.
#include "dect2/rpr/rpr_gen.h"
#include "dect2/rpr/rpr_rx.h"
#include "dect2/rpr/rpr_tel.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static const double kRate = 250000;

static RprTelemetry run(RprGenConfig c, double sec, bool noiseOnly = false) {
    c.rate = kRate; c.analytic = true; c.dialOffsetHz = -20000 + 0;
    RprGenerator g(c);
    RprReceiver r;
    r.configure(kRate);
    r.setSignalOffset(-20000);
    r.setAnalysisInterval(8);
    std::mt19937 rng(3);
    std::normal_distribution<float> nd(0.f, 0.02f);
    std::vector<cf32> buf(25000);
    const size_t tot = (size_t)(sec * kRate);
    for (size_t i = 0; i < tot; i += buf.size()) {
        if (noiseOnly) for (auto& v : buf) v = cf32(nd(rng), nd(rng)); else g.generate(buf.data(), buf.size());
        r.feed(buf.data(), buf.size());
    }
    r.flush();
    RprTelemetry t;
    r.telemetry(t, 0);
    return t;
}

int main() {
    for (int lv = 1; lv <= 2; lv++) {
        RprGenConfig c; c.level = lv; c.snrDb = 20;
        const RprTelemetry t = run(c, 30);
        printf("  level %d: %s\n", lv, rprSummary(t).c_str());
        CHECK(t.state == 2 && t.dataValid && t.level == lv, "level %d: state %d level %d", lv, t.state, t.level);
        CHECK(t.blocksOk >= 4 && t.bursts.size() >= 4, "level %d: %llu bursts", lv, (unsigned long long)t.blocksOk);
        CHECK(std::fabs(t.centreHz - 1500) < 4 && t.carrierHz.size() == 8, "level %d: centre %.1f", lv, t.centreHz);
        CHECK(t.rows.size() == 3 && !t.rows[0].empty(), "level %d: symbol rows %zu", lv, t.rows.size());
        CHECK(!t.events.empty() && !t.bursts.front().bits.empty() && t.bursts.front().level == lv, "level %d: events and burst bits", lv);
        CHECK(rprSummary(t).find("needs a recording") != std::string::npos, "the summary says the frames are not decoded");
    }
    {   // the radio tuned 50 Hz off, both ways
        for (double off : {-50.0, 50.0}) {
            RprGenConfig c; c.level = 2; c.snrDb = 15; c.cfoHz = off;
            const RprTelemetry t = run(c, 30);
            CHECK(t.level == 2 && t.blocksOk >= 3 && std::fabs(t.cfoHz - off) < 4, "tuned %+.0f Hz: level %d bursts %llu cfo %.1f", off, t.level, (unsigned long long)t.blocksOk, t.cfoHz);
        }
    }
    {   // noise alone
        RprGenConfig c;
        const RprTelemetry t = run(c, 30, true);
        CHECK(t.state == 0 && t.events.empty() && t.blocksOk == 0, "noise: state %d, %llu bursts", t.state, (unsigned long long)t.blocksOk);
    }
    {
        RprReceiver r;
        r.configure(100000);
        CHECK(!r.ready(), "100 ksps is below the minimum");
    }
    return testSummary("rpr rx");
}
