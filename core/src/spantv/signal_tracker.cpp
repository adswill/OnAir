#include "dect2/engine/spantv/signal_tracker.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

const std::vector<TrackedSignal>& SignalTracker::update(const std::vector<SignalCand>& cands) {
    std::vector<bool> used(cands.size(), false);
    for (TrackedSignal& t : tracks_) {
        // the nearest unused candidate whose centre and width fit
        size_t best = SIZE_MAX;
        double bestD = 1e300;
        const double tol = std::max(t.cand.bw3Hz / 4, 2 * binHz_);
        for (size_t i = 0; i < cands.size(); i++) {
            if (used[i]) continue;
            const double d = std::fabs(cands[i].centerHz - t.cand.centerHz);
            const double ratio = t.cand.bw3Hz > 0 ? cands[i].bw3Hz / t.cand.bw3Hz : 1;
            if (d > tol || ratio < 1 / 1.5 || ratio > 1.5) continue;
            if (d < bestD) { bestD = d; best = i; }
        }
        if (best == SIZE_MAX) { t.misses++; t.hits = 0; continue; }   // still "present" until the owner prunes it (a short fade)
        used[best] = true;
        const SignalCand& c = cands[best];
        // smooth the estimates (the first ones count fully)
        const double a = t.hits + t.misses == 0 ? 1.0 : 0.3;
        auto mix = [a](double o, double n) { return o + a * (n - o); };
        t.cand.centerHz = mix(t.cand.centerHz, c.centerHz);
        t.cand.bw3Hz = mix(t.cand.bw3Hz, c.bw3Hz);
        t.cand.bwHz = mix(t.cand.bwHz, c.bwHz);
        t.cand.edgeHz = mix(t.cand.edgeHz, c.edgeHz);
        t.cand.rsEstimate = mix(t.cand.rsEstimate, c.rsEstimate);
        t.cand.plateauDb = c.plateauDb; t.cand.floorDb = c.floorDb; t.cand.snrDb = c.snrDb;
        if (c.rollOff > 0) t.cand.rollOff = c.rollOff;
        t.cand.clipped = c.clipped;
        t.hits++; t.misses = 0;
        if (t.hits >= 2) t.present = true;
    }
    for (size_t i = 0; i < cands.size(); i++) {
        if (used[i]) continue;
        TrackedSignal t;
        t.id = nextId_++;
        t.cand = cands[i];
        t.hits = 1;
        tracks_.push_back(t);
    }
    std::sort(tracks_.begin(), tracks_.end(), [](const TrackedSignal& a, const TrackedSignal& b) { return a.cand.centerHz < b.cand.centerHz; });
    return tracks_;
}

void SignalTracker::prune(int maxMisses, const std::function<bool(int)>& keep) {
    tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(), [&](const TrackedSignal& t) {
        if (!t.present && t.misses >= 2) return !(keep && keep(t.id));   // seen once, then gone: noise
        return t.misses >= maxMisses && !(keep && keep(t.id));
    }), tracks_.end());
}

const TrackedSignal* SignalTracker::find(int id) const {
    for (const TrackedSignal& t : tracks_) if (t.id == id) return &t;
    return nullptr;
}

} // namespace dect2
