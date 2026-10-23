// Whole-span TV: the signals of the span over time (docs/WHOLESPAN-TV-DESIGN.md 1.2). Each evaluation's candidates are matched to the known
// signals by centre (within a quarter of the width, at least two bins) and width (within a factor 1.5); a signal gets a stable id, is "present"
// after two evaluations in a row and counts the evaluations it was missing. The owner removes it (prune) once it has been missing long enough
// and no receiver holds on to it.
#pragma once
#include "adapter.h"
#include <functional>
#include <vector>

namespace dect2 {

struct TrackedSignal {
    int id = 0;
    SignalCand cand;          // smoothed estimates
    int hits = 0;             // evaluations it was seen in (in a row)
    int misses = 0;           // evaluations in a row it was missing
    bool present = false;     // seen twice in a row (and not missing for long)
};

class SignalTracker {
public:
    void reset() { tracks_.clear(); }
    void setBinHz(double b) { binHz_ = b; }
    // one evaluation; returns the tracks after it (sorted by frequency)
    const std::vector<TrackedSignal>& update(const std::vector<SignalCand>& cands);
    // drops the tracks missing for at least maxMisses evaluations unless keep(id) says no
    void prune(int maxMisses, const std::function<bool(int id)>& keep);
    const std::vector<TrackedSignal>& tracks() const { return tracks_; }
    const TrackedSignal* find(int id) const;

private:
    std::vector<TrackedSignal> tracks_;
    double binHz_ = 0;
    int nextId_ = 0;
};

} // namespace dect2
