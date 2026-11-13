// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF propagation dashboard model: band mapping, the 30 minute "now" judgement, the 24 h history, the best band, save / load.
#include "dect2/hfbeacon/hfbeacon_prop.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    using namespace dect2::hfp;
    CHECK(bandOf(14.0956e6) == 5 && bandOf(14.1e6) == 5 && bandOf(28.2e6) == 9 && bandOf(7.0386e6) == 3, "bandOf");
    CHECK(bandOf(11.999e6) == -1 && bandOf(1e6) == -1 && bandOf(50.313e6) == 10, "bandOf outside");

    const double t0 = 1.8e9 - std::fmod(1.8e9, 900.0);   // a bin start
    Model m;
    // 20 m: six FT8 stations in the last 20 minutes, one far
    for (int i = 0; i < 6; i++) {
        Obs o; o.utc = t0 + 60 * i; o.hz = 14.074e6; o.source = Ft8; o.id = "DL" + std::to_string(i); o.snrDb = -10 + i; o.distKm = 1500 + 1500 * i;
        m.add(o);
    }
    // 40 m: one WSPR spot; 10 m: nothing
    { Obs o; o.utc = t0 + 30; o.hz = 7.0386e6; o.source = Wspr; o.id = "SM0X"; o.snrDb = -22; o.distKm = 1200; m.add(o); }
    // an observation outside every band is ignored
    { Obs o; o.utc = t0; o.hz = 11.999e6; o.source = Dashes; o.id = "ld"; m.add(o); }
    CHECK(m.total() == 7, "total %llu", (unsigned long long)m.total());

    const double now = t0 + 600;
    auto v = m.now(now);
    CHECK(v[5].state == Open && v[5].stations == 6 && v[5].maxKm == 9000 && v[5].bySource[Ft8] == 6, "20 m %d %d", v[5].state, v[5].stations);
    CHECK(v[3].state == Marginal && v[3].stations == 1, "40 m");
    CHECK(v[9].state == Closed && v[9].score == 0, "10 m closed");
    std::string why;
    CHECK(m.best(now, &why) == 5 && !why.empty(), "best %s", why.c_str());

    // 50 minutes later the 30 minute window has passed: everything closed, no best band
    v = m.now(t0 + 3000);
    CHECK(v[5].state == Closed && m.best(t0 + 3000) == -1, "window ends");
    // the history still has the bin, at the right place (24 h grid, newest last)
    auto h = m.history(5, t0 + 3000);
    CHECK(h.size() == 96 && h[95].count == 0 && h[92].count == 6 && h[92].stations == 6, "history %d", h[92].count);
    CHECK(m.history(5, t0 + 25 * 3600)[95].count == 0 && m.history(5, t0 + 25 * 3600)[0].count == 0, "stale");

    // a slot reused a day later replaces the old bin
    { Obs o; o.utc = t0 + 86400 + 10; o.hz = 14.074e6; o.source = Ft4; o.id = "G3"; o.snrDb = 3; m.add(o); }
    h = m.history(5, t0 + 86400 + 10);
    CHECK(h[95].count == 1 && h[95].stations == 1, "reuse");
    { Obs o; o.utc = t0 + 5; o.hz = 14.074e6; o.source = Ft4; o.id = "late"; m.add(o); }   // older than the slot holds
    CHECK(m.history(5, t0 + 86400 + 10)[95].count == 1, "old ignored");

    // save / load keeps counts; damaged lines and old bins are dropped
    std::string s = m.save();
    Model m2; m2.load(s + "garbage line\n3 1 2\n", t0 + 86400 + 60);
    h = m2.history(5, t0 + 86400 + 60);
    CHECK(h[95].count == 1 && m2.total() == 1, "load %d %llu", h[95].count, (unsigned long long)m2.total());
    Model m3; m3.load("not a file", t0); CHECK(m3.total() == 0, "bad header");
    // the beacon sources count together with spots
    Model m4;
    for (int i = 0; i < 3; i++) { Obs o; o.utc = t0 + i; o.hz = 21.15e6; o.source = Ncdxf; o.id = "B" + std::to_string(i); o.snrDb = 12; o.distKm = 8000; m4.add(o); }
    CHECK(m4.now(t0 + 10)[7].state == Open, "3 beacons open");
    return testSummary("hfbeacon_prop");
}
