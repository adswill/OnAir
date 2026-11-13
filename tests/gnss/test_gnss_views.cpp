// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The satellite-system filter of the GNSS receiver (GnssReceiver::setSystems, the Systems choice of the GNSS screen): one system wanted. A simulated sky with GPS, Galileo and QZSS satellites on L1 (4 Msps, 8-bit samples, 45 s)
// is run through a receiver with every system wanted and through one for GPS and one for QZSS: the view tracks its own system and no other (no locked channel of another system).
// GLONASS and BeiDou use the same mask and the receiver's own tests cover their signals. Simulated signals only.
#include "dect2/gnss/gnss_testkit.h"
#include "dect2/gnss/gnss_rx.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <algorithm>
#include <string>
using namespace dect2;
using namespace dect2::gnsstest;
#include "../common/test_util.h"

static std::set<int> lockedPrns(const GnssTelemetry& t, int sys) { std::set<int> s; for (const auto& c : t.channels) if (c.sys == sys && c.state >= GnssChLocked) s.insert(c.prn); return s; }
static int lockedOther(const GnssTelemetry& t, int sys) { int n = 0; for (const auto& c : t.channels) if (c.sys != sys && c.state >= GnssChLocked) n++; return n; }

int main() {
    GnssSimConfig cfg;
    cfg.systems = gnssSystemBit(GnssGps) | gnssSystemBit(GnssGalileo) | gnssSystemBit(GnssQzss);
    struct W { const char* name; unsigned mask; int sys; size_t minLocked; };
    const W ws[] = {{"all systems", 0xFFFFFFFFu, -1, 0}, {"GPS", gnssSystemBit(GnssGps), GnssGps, 3}, {"QZSS", gnssSystemBit(GnssQzss), GnssQzss, 1}};
    std::set<int> all[6];
    for (const W& w : ws) {
        GnssSim sim(cfg, 4e6);
        Options o; o.rate = 4e6; o.secs = 45; o.chunk = 64000;
        const unsigned mask = w.mask;
        o.setup = [mask](GnssReceiver& rx) { rx.setSystems(mask); };
        const Run r = run(sim, o);
        // the satellites locked at any report
        std::set<int> seen[6]; int others = 0;
        for (const auto& rep : r.reports) for (const auto& c : rep.channels) if (c.state >= GnssChLocked) { seen[c.sys].insert(c.prn); if (w.sys >= 0 && c.sys != w.sys) others++; }
        printf("  %-12s locked at some report: GPS %zu, Galileo %zu, QZSS %zu, other systems %d\n", w.name, seen[GnssGps].size(), seen[GnssGalileo].size(), seen[GnssQzss].size(), others);
        if (w.sys < 0) { for (int s = 0; s < 6; s++) all[s] = seen[s]; CHECK(seen[GnssGps].size() >= 4 && seen[GnssGalileo].size() >= 2 && seen[GnssQzss].size() >= 1, "every system: GPS %zu, Galileo %zu, QZSS %zu", seen[GnssGps].size(), seen[GnssGalileo].size(), seen[GnssQzss].size()); }
        else {
            CHECK(others == 0, "%s view: %d locked channels of other systems", w.name, others);
            CHECK(seen[w.sys].size() >= w.minLocked, "%s view: %zu satellites locked (want %zu)", w.name, seen[w.sys].size(), w.minLocked);
            CHECK(lockedOther(r.tel, w.sys) == 0, "%s view: the last report has channels of other systems", w.name);
        }
    }
    return testSummary("gnss views");
}
