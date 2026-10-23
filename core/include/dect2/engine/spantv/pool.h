// Whole-span TV: which signals get a receiver (docs/WHOLESPAN-TV-DESIGN.md 1.5). A pure decision, tested on its own: the signals in priority
// order (1 the selected one, 2 pinned ones: a recording, a stream or a network tuner client, 3 running ones that locked, 4 other running ones,
// 5 the rest by SNR; a receiver that never locked within its time drops to the end), then taken while the count stays within the cap and the
// predicted CPU within the budget. The selected signal always runs. A running receiver younger than the minimum run time is not stopped for a
// stronger newcomer; a signal stopped for the budget is not tried again for a while.
#pragma once
#include <vector>

namespace dect2 {

struct PoolEntry {
    int id = 0;
    bool running = false;
    bool selected = false;
    bool pinned = false;          // priority 2: something records or streams it
    bool locked = false;          // is or was locked
    bool noLock = false;          // ran for its lock time without locking
    bool manualOff = false;       // the user switched decoding of this signal off
    double snrDb = 0;
    float cost = 0;               // CPU cores it needs (measured when running for long enough, else the adapter's estimate)
    double runSecs = 0;           // how long its receiver runs
    double sinceBudgetStop = 1e9; // seconds since it was last stopped for the budget
};

struct PoolLimits {
    int maxReceivers = 2;         // hard cap (at least 1)
    double budgetCores = 0;       // <= 0: no CPU budget, the cap only
    bool decodeAll = true;        // false: the selected signal only
    double minRunSecs = 5;
    double retrySecs = 10;
};

struct PoolPlan {
    std::vector<int> start, stop, keep;   // ids
    double plannedCores = 0;
    std::vector<int> waiting;             // wanted but held back by the cap or the budget
};

PoolPlan planPool(std::vector<PoolEntry> entries, const PoolLimits& lim);

// The default CPU budget: 0.7 of the cores less one for the analysis and the interface (at least one core)
double defaultPoolBudget(unsigned cores);

} // namespace dect2
