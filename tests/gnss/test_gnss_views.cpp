// The satellite-system entries (families 860, 866, 872, 875; gnss_rx.h: gnssViews): the GNSS receiver with one system wanted. A simulated sky with GPS, Galileo and QZSS satellites on L1 (4 Msps, 8-bit samples, 45 s)
// is run through a receiver with every system wanted and through one for each of the three views: the view tracks its own system and no other (no locked channel of another system). The tables (numbers, lookups, tunings: GLONASS 10 Msps at 1602 MHz, BeiDou 1561.098 MHz) are checked for all four; the GLONASS and BeiDou entries use the same mask as the others
// and the receiver's own tests cover their signals. Simulated signals only.
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
    int nv = 0;
    const GnssView* views = gnssViews(nv);
    CHECK(nv == 4, "%d views", nv);
    for (int i = 0; i < nv; i++) {
        CHECK(gnssViewForActive(views[i].family + 1) == &views[i] && gnssViewForStd(views[i].family + 2) == &views[i], "view %d lookup", i);
        const ModeTuning t = gnssViewTuning(i);
        CHECK(t.stdMode == views[i].family + 2 && std::string(t.id) == views[i].id && std::fabs(t.defMhz - views[i].defMhz) < 1e-9, "view %d tuning", i);
        CHECK(gnssTuneOf(views[i].defMhz) != GnssTuneCustom, "view %d: %.3f MHz is a tuning of the receiver", i, views[i].defMhz);
        CHECK(std::fabs(t.sampleRate - gnssAutoRate(views[i].defMhz)) < 1.0, "view %d: sample rate %.0f, automatic %.0f", i, t.sampleRate, gnssAutoRate(views[i].defMhz));
        for (int j = 0; j < i; j++) CHECK(views[j].family != views[i].family && std::strcmp(views[j].id, views[i].id) != 0 && views[j].system != views[i].system, "views %d and %d clash", j, i);
    }
    CHECK(gnssViewForActive(13) == nullptr && gnssViewForStd(14) == nullptr, "the GNSS mode is not a view");

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
