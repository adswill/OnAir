// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pass scheduler: priority on overlaps, trimming, busy windows, short slots, daily windows.
#include "dect2/geo/pass_sched.h"
#include <cmath>
#include <cstdio>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static SchedPass sp(int sat, int prio, double aos, double los, double el) {
    SchedPass p; p.sat = sat; p.priority = prio; p.pass.aos = aos; p.pass.los = los; p.pass.tca = (aos + los) / 2; p.pass.maxElDeg = el; return p;
}

int main() {
    SchedParams prm; prm.leadSec = 20; prm.minUsefulSec = 120;
    {   // no overlap: all taken, in order
        const SchedPlan pl = planPasses({sp(1, 0, 2000, 2600, 40), sp(0, 0, 1000, 1600, 30)}, {}, 0, prm);
        CHECK(pl.slots.size() == 2 && pl.slots[0].sp.sat == 0 && pl.slots[1].sp.sat == 1 && !pl.slots[0].trimmed, "plain %zu", pl.slots.size());
    }
    {   // overlap: the higher priority wins even with a lower elevation; the other is trimmed after it when 120 s are left
        const SchedPlan pl = planPasses({sp(0, 1, 1000, 1600, 20), sp(1, 5, 1300, 2100, 80)}, {}, 0, prm);
        CHECK(pl.slots.size() == 2 && pl.slots[0].sp.sat == 0 && pl.slots[1].sp.sat == 1, "prio order");
        CHECK(pl.slots[0].end <= 1300 - 20 + 1e-6 && pl.slots[0].trimmed == true && pl.slots[0].start == 1000, "low trimmed before: %.0f-%.0f", pl.slots[0].start, pl.slots[0].end);
        CHECK(!pl.slots[1].trimmed, "winner whole");
    }
    {   // the loser has too little left: dropped as Conflict
        const SchedPlan pl = planPasses({sp(0, 1, 1000, 1400, 20), sp(1, 5, 1100, 1900, 80)}, {}, 0, prm);
        CHECK(pl.slots.size() == 1 && pl.slots[0].sp.sat == 1 && pl.dropped.size() == 1 && pl.dropped[0].why == DropReason::Conflict, "conflict %zu %zu", pl.slots.size(), pl.dropped.size());
    }
    {   // equal priority: the higher elevation wins
        const SchedPlan pl = planPasses({sp(0, 0, 1000, 1700, 25), sp(1, 0, 1000, 1700, 70)}, {}, 0, prm);
        CHECK(pl.slots.size() == 1 && pl.slots[0].sp.sat == 1, "elevation tie-break");
    }
    {   // lead time: the second pass starts right when the first ends, so the first gives way by the lead
        const SchedPlan pl = planPasses({sp(0, 9, 1000, 1500, 50), sp(1, 0, 1500, 2200, 50)}, {}, 0, prm);
        CHECK(pl.slots.size() == 2 && std::fabs(pl.slots[1].start - 1520) < 1e-6 && pl.slots[1].trimmed, "lead %.0f", pl.slots.size() > 1 ? pl.slots[1].start : -1.0);
    }
    {   // busy window: a pass inside is dropped as Busy, one overlapping its start is trimmed
        const SchedPlan pl = planPasses({sp(0, 0, 1000, 1500, 50), sp(1, 0, 3000, 3600, 50)}, {{900, 1700}, {3000, 3300}}, 0, prm);
        CHECK(pl.dropped.size() == 1 && pl.dropped[0].sp.sat == 0 && pl.dropped[0].why == DropReason::Busy, "busy drop");
        CHECK(pl.slots.size() == 1 && std::fabs(pl.slots[0].start - 3320) < 1e-6 && std::fabs(pl.slots[0].end - 3600) < 1e-6, "busy trim %.0f", pl.slots.empty() ? -1.0 : pl.slots[0].start);
    }
    {   // past passes ignored, a pass under way starts now; short and low passes dropped
        const SchedPlan pl = planPasses({sp(0, 0, 100, 500, 50), sp(1, 0, 900, 1500, 50), sp(2, 0, 2000, 2060, 50), sp(3, 0, 3000, 3600, 4)}, {}, 1000, {20, 120, 10});
        CHECK(pl.slots.size() == 1 && pl.slots[0].sp.sat == 1 && pl.slots[0].start == 1000 && pl.slots[0].trimmed, "now");
        bool shortOk = false, lowOk = false;
        for (const auto& d : pl.dropped) { if (d.sp.sat == 2 && d.why == DropReason::TooShort) shortOk = true; if (d.sp.sat == 3 && d.why == DropReason::LowElevation) lowOk = true; }
        CHECK(shortOk && lowOk, "drops");
    }
    {   // daily windows, UTC+3: 23:00-01:00 local; day 0 of the epoch
        const auto b = dailyBusy(23 * 60, 60, 0, 2 * 86400, 3 * 3600);
        bool ok = false;
        for (const auto& w : b) if (std::fabs(w.start - (23 * 3600 - 3 * 3600)) < 1e-6 && std::fabs(w.end - w.start - 7200) < 1e-6) ok = true;
        CHECK(ok && b.size() >= 2, "daily %zu", b.size());
        CHECK(dailyBusy(5, 5, 0, 1e5, 0).empty(), "empty window");
    }
    if (!fails) printf("pass_sched: all ok\n");
    return fails ? 1 : 0;
}
