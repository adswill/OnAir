// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Is the storm coming or going, from one station. Strike rate and median strike strength (relative to this receiver's own floor, so stable for this station) over two
// five-minute halves of the last ten minutes. A trend, not a position: a strengthening series means nearer or a stronger cell, and either way it deserves attention.
#pragma once
#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>

namespace dect2 {

enum class StormPhase { None, Distant, Approaching, Overhead, Receding, Steady };
inline const char* stormPhaseName(StormPhase p) {
    switch (p) {
    case StormPhase::Distant: return "distant storm";
    case StormPhase::Approaching: return "storm approaching";
    case StormPhase::Overhead: return "storm overhead";
    case StormPhase::Receding: return "storm receding";
    case StormPhase::Steady: return "storm steady";
    default: return "no storm";
    }
}

class StormTrend {
public:
    void reset() { s_.clear(); best_ = 0; }
    void add(double t, float peakDb) { s_.push_back({t, peakDb}); best_ = std::max(best_, peakDb); }
    // now: signal seconds; ratePerMin and dbChange: the last five minutes against the five before
    StormPhase update(double now) {
        while (!s_.empty() && s_.front().t < now - 600.0) s_.pop_front();
        std::vector<float> a, b;
        for (const auto& e : s_) (e.t < now - 300.0 ? a : b).push_back(e.db);
        rateNow = b.size() / 5.0; ratePrev = a.size() / 5.0;
        medNow = med(b); medPrev = med(a);
        dbChange = (a.size() >= 4 && b.size() >= 4) ? medNow - medPrev : 0;
        if (s_.size() < 5) return last = StormPhase::None;
        if (rateNow >= 12 && best_ - medNow <= 6.0f) return last = StormPhase::Overhead;
        if (a.size() >= 4 && b.size() >= 4) {
            const bool stronger = dbChange >= 2.5f, weaker = dbChange <= -2.5f, more = rateNow >= ratePrev * 1.4, fewer = rateNow <= ratePrev * 0.6;
            if ((stronger && !fewer) || (more && !weaker && dbChange >= 0.5f)) return last = StormPhase::Approaching;
            if ((weaker && !more) || (fewer && dbChange <= -0.5f)) return last = StormPhase::Receding;
            return last = StormPhase::Steady;
        }
        return last = s_.size() >= 5 && rateNow < 3 ? StormPhase::Distant : StormPhase::Steady;
    }
    double rateNow = 0, ratePrev = 0; float medNow = 0, medPrev = 0, dbChange = 0;
    StormPhase last = StormPhase::None;
private:
    static float med(std::vector<float> v) { if (v.empty()) return 0; std::nth_element(v.begin(), v.begin() + (long)v.size() / 2, v.end()); return v[v.size() / 2]; }
    struct E { double t; float db; };
    std::deque<E> s_;
    float best_ = 0;
};

} // namespace dect2
