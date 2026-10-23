#include "dect2/engine/spantv/pool.h"
#include <algorithm>

namespace dect2 {

double defaultPoolBudget(unsigned cores) { return std::max(1.0, 0.7 * (double)cores - 1.0); }

namespace {
int rank(const PoolEntry& e) {
    if (e.selected) return 0;
    if (e.pinned) return 1;
    if (e.noLock) return 6;
    if (e.running && e.locked) return 2;
    if (e.running) return 3;
    if (e.locked) return 4;
    return 5;
}
}

PoolPlan planPool(std::vector<PoolEntry> entries, const PoolLimits& lim) {
    PoolPlan plan;
    std::stable_sort(entries.begin(), entries.end(), [](const PoolEntry& a, const PoolEntry& b) {
        const int ra = rank(a), rb = rank(b);
        if (ra != rb) return ra < rb;
        return a.snrDb > b.snrDb;
    });
    const int cap = std::max(1, lim.maxReceivers);
    const bool budget = lim.budgetCores > 0;
    int count = 0;
    double cores = 0;
    std::vector<bool> take(entries.size(), false);
    // first pass: what fits, in priority order
    for (size_t i = 0; i < entries.size(); i++) {
        const PoolEntry& e = entries[i];
        if (e.manualOff && !e.selected) continue;
        if (!lim.decodeAll && !e.selected && !e.pinned) continue;
        if (!e.running && !e.selected && e.sinceBudgetStop < lim.retrySecs) { plan.waiting.push_back(e.id); continue; }
        const bool fitsCap = count < cap;
        const bool fitsBudget = !budget || cores + e.cost <= lim.budgetCores + 1e-9;
        if (e.selected || (fitsCap && fitsBudget)) { take[i] = true; count++; cores += e.cost; }
        else plan.waiting.push_back(e.id);
    }
    // a young receiver keeps running while the cap and budget allow it (a newcomer that pushed it out waits instead)
    for (size_t i = 0; i < entries.size(); i++) {
        const PoolEntry& e = entries[i];
        if (take[i] || !e.running || e.runSecs >= lim.minRunSecs || (e.manualOff && !e.selected) || (!lim.decodeAll && !e.pinned)) continue;
        // displace the lowest-priority newcomer that was taken
        for (size_t j = entries.size(); j-- > 0;) {
            if (!take[j] || entries[j].running || entries[j].selected) continue;
            if (budget && cores - entries[j].cost + e.cost > lim.budgetCores + 1e-9) continue;
            take[j] = false; cores -= entries[j].cost; plan.waiting.push_back(entries[j].id);
            take[i] = true; cores += e.cost;
            break;
        }
    }
    for (size_t i = 0; i < entries.size(); i++) {
        const PoolEntry& e = entries[i];
        if (take[i]) (e.running ? plan.keep : plan.start).push_back(e.id);
        else if (e.running) plan.stop.push_back(e.id);
    }
    plan.plannedCores = cores;
    return plan;
}

} // namespace dect2
