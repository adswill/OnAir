// Whole-span TV: the receiver pool's decision (spantv/pool.h) with made-up signals: the cap, the CPU budget, the selected signal first and always,
// pinned (recorded) signals next, running receivers not thrown out for a newcomer before their minimum run time, the retry hold after a budget stop,
// "selected only", the user's own off switch and a receiver that never locked giving way.
#include "dect2/engine/spantv/pool.h"
#include <algorithm>
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

static bool has(const std::vector<int>& v, int id) { return std::find(v.begin(), v.end(), id) != v.end(); }
static PoolEntry E(int id, double snr, float cost, bool running = false, double runSecs = 0) {
    PoolEntry e; e.id = id; e.snrDb = snr; e.cost = cost; e.running = running; e.runSecs = runSecs; return e;
}

int main() {
    {   // cap 2, no budget: the two strongest start
        PoolLimits l; l.maxReceivers = 2;
        auto p = planPool({E(1, 10, 0.5f), E(2, 20, 0.5f), E(3, 15, 0.5f)}, l);
        CHECK(p.start.size() == 2 && has(p.start, 2) && has(p.start, 3), "cap 2: %zu started", p.start.size());
        CHECK(has(p.waiting, 1), "the weakest waits");
    }
    {   // the selected one runs even when it is the weakest and the cap is full
        PoolLimits l; l.maxReceivers = 2;
        auto a = E(1, 5, 0.5f); a.selected = true;
        auto p = planPool({a, E(2, 20, 0.5f), E(3, 15, 0.5f)}, l);
        CHECK(has(p.start, 1) && has(p.start, 2) && !has(p.start, 3), "selected first");
    }
    {   // budget 1.5 cores: 1.0 + 0.4 fit, the next 0.6 does not, a cheaper 0.1 after it does
        PoolLimits l; l.maxReceivers = 8; l.budgetCores = 1.5;
        auto p = planPool({E(1, 30, 1.0f), E(2, 25, 0.4f), E(3, 20, 0.6f), E(4, 10, 0.1f)}, l);
        CHECK(has(p.start, 1) && has(p.start, 2) && !has(p.start, 3) && has(p.start, 4), "budget: 1 2 4 (%.2f cores)", p.plannedCores);
        CHECK(p.plannedCores <= 1.5 + 1e-9, "planned %.2f", p.plannedCores);
    }
    {   // the selected one is never refused for the budget
        PoolLimits l; l.maxReceivers = 4; l.budgetCores = 1.0;
        auto a = E(1, 10, 2.5f); a.selected = true;
        auto p = planPool({a, E(2, 25, 0.4f)}, l);
        CHECK(has(p.start, 1) && !has(p.start, 2), "a big selected transponder takes the whole budget");
    }
    {   // pinned beats strong
        PoolLimits l; l.maxReceivers = 2;
        auto a = E(1, 30, 0.5f); a.selected = true;
        auto b = E(3, 5, 0.5f); b.pinned = true;
        auto p = planPool({a, E(2, 25, 0.5f), b}, l);
        CHECK(has(p.start, 1) && has(p.start, 3) && !has(p.start, 2), "the recorded signal keeps its receiver");
    }
    {   // running receivers stay; a stronger newcomer waits (cap full)
        PoolLimits l; l.maxReceivers = 2;
        auto p = planPool({E(1, 10, 0.5f, true, 60), E(2, 12, 0.5f, true, 60), E(3, 30, 0.5f)}, l);
        CHECK(has(p.keep, 1) && has(p.keep, 2) && p.start.empty() && p.stop.empty(), "running ones stay");
    }
    {   // the user selects a new signal: the weakest old one gives way once it ran for its minimum time
        PoolLimits l; l.maxReceivers = 2;
        auto n = E(3, 8, 0.5f); n.selected = true;
        auto p = planPool({E(1, 10, 0.5f, true, 60), E(2, 12, 0.5f, true, 60), n}, l);
        CHECK(has(p.start, 3) && p.stop.size() == 1 && has(p.stop, 1), "the weakest stops for the selected one");
    }
    {   // a young receiver is not thrown out for a non-selected newcomer of higher rank (budget)
        PoolLimits l; l.maxReceivers = 4; l.budgetCores = 1.0;
        auto old = E(1, 5, 0.6f, true, 2);   // running 2 s, not locked
        auto lk = E(2, 9, 0.6f); lk.locked = true;   // locked before, not running: rank above a fresh unlocked one... but young old stays
        auto p = planPool({old, lk}, l);
        CHECK(has(p.keep, 1) && !has(p.start, 2), "a 2 s old receiver keeps its place");
    }
    {   // a signal stopped for the budget 4 s ago waits for its retry time
        PoolLimits l; l.maxReceivers = 4;
        auto a = E(1, 20, 0.5f); a.sinceBudgetStop = 4;
        auto p = planPool({a, E(2, 10, 0.5f)}, l);
        CHECK(!has(p.start, 1) && has(p.start, 2) && has(p.waiting, 1), "retry hold");
    }
    {   // selected only
        PoolLimits l; l.maxReceivers = 4; l.decodeAll = false;
        auto a = E(2, 10, 0.5f); a.selected = true;
        auto p = planPool({E(1, 20, 0.5f, true, 60), a, E(3, 30, 0.5f)}, l);
        CHECK(p.start.size() == 1 && has(p.start, 2) && has(p.stop, 1), "selected only: one receiver");
    }
    {   // the user's off switch and a receiver that never locked
        PoolLimits l; l.maxReceivers = 2;
        auto off = E(1, 30, 0.5f); off.manualOff = true;
        auto nl = E(2, 25, 0.5f, true, 40); nl.noLock = true;
        auto p = planPool({off, nl, E(3, 10, 0.5f), E(4, 9, 0.5f)}, l);
        CHECK(!has(p.start, 1), "switched off");
        CHECK(has(p.start, 3) && has(p.start, 4) && has(p.stop, 2), "the never-locked receiver gives way to two waiting signals");
    }
    CHECK(defaultPoolBudget(10) > 5.9 && defaultPoolBudget(10) < 6.1 && defaultPoolBudget(2) == 1.0, "default budget");
    return testSummary("spantv_pool");
}
