// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/geo/pass_sched.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

namespace {
struct Iv { double a, b; };

// The longest piece of [a, b] that none of the blocked intervals touch
Iv longestFree(double a, double b, std::vector<Iv> blocked) {
    std::sort(blocked.begin(), blocked.end(), [](const Iv& x, const Iv& y) { return x.a < y.a; });
    Iv best{a, a};
    double cur = a;
    for (const Iv& v : blocked) {
        if (v.b <= cur) continue;
        if (v.a >= b) break;
        if (v.a - cur > best.b - best.a) best = {cur, v.a};
        cur = std::max(cur, v.b);
        if (cur >= b) break;
    }
    if (cur < b && b - cur > best.b - best.a) best = {cur, b};
    return best;
}
}  // namespace

SchedPlan planPasses(const std::vector<SchedPass>& passes, const std::vector<BusyWindow>& busy, double nowSec, const SchedParams& prm) {
    SchedPlan plan;
    std::vector<size_t> order;
    for (size_t i = 0; i < passes.size(); i++) {
        if (passes[i].pass.los <= nowSec || passes[i].pass.los <= passes[i].pass.aos) continue;
        if (passes[i].pass.maxElDeg < prm.minElDeg) plan.dropped.push_back({passes[i], DropReason::LowElevation});
        else order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) {
        const SchedPass &p = passes[x], &q = passes[y];
        if (p.priority != q.priority) return p.priority > q.priority;
        if (p.pass.maxElDeg != q.pass.maxElDeg) return p.pass.maxElDeg > q.pass.maxElDeg;
        return p.pass.aos < q.pass.aos;
    });
    std::vector<Iv> taken;   // claimed intervals, lead time included
    for (const BusyWindow& b : busy) if (b.end > b.start) taken.push_back({b.start, b.end});
    const size_t nBusy = taken.size();
    for (size_t i : order) {
        const SchedPass& sp = passes[i];
        const double a = std::max(sp.pass.aos, nowSec);
        // the lead time is needed before the slot, so the free search starts one lead earlier (the claimed intervals are widened)
        std::vector<Iv> blocked;
        for (const Iv& t : taken) blocked.push_back({t.a, t.b + prm.leadSec});
        const Iv f = longestFree(a, sp.pass.los, blocked);
        if (f.b - f.a < prm.minUsefulSec) {
            // busy only, or another pass?
            std::vector<Iv> onlyBusy(taken.begin(), taken.begin() + nBusy);
            for (Iv& v : onlyBusy) v.b += prm.leadSec;
            const Iv g = longestFree(a, sp.pass.los, onlyBusy);
            DropReason why = DropReason::Conflict;
            if (sp.pass.los - a < prm.minUsefulSec) why = DropReason::TooShort;
            else if (g.b - g.a < prm.minUsefulSec) why = DropReason::Busy;
            plan.dropped.push_back({sp, why});
            continue;
        }
        SchedSlot s;
        s.sp = sp; s.start = f.a; s.end = f.b;
        s.trimmed = f.a > sp.pass.aos + 0.5 || f.b < sp.pass.los - 0.5;
        // a slot that starts at the pass's own AOS claims the lead before it; a trimmed start already sits after the lead of the earlier job
        taken.push_back({s.start - prm.leadSec, s.end});
        plan.slots.push_back(s);
    }
    std::sort(plan.slots.begin(), plan.slots.end(), [](const SchedSlot& x, const SchedSlot& y) { return x.start < y.start; });
    std::sort(plan.dropped.begin(), plan.dropped.end(), [](const SchedDropped& x, const SchedDropped& y) { return x.sp.pass.aos < y.sp.pass.aos; });
    return plan;
}

std::vector<BusyWindow> dailyBusy(int fromMin, int toMin, double t0, double t1, double utcOffsetSec) {
    std::vector<BusyWindow> out;
    if (fromMin == toMin || fromMin < 0 || toMin < 0 || fromMin >= 1440 || toMin > 1440) return out;
    const double day0 = std::floor((t0 + utcOffsetSec) / 86400.0) * 86400.0 - utcOffsetSec - 86400.0;
    for (double d = day0; d < t1; d += 86400.0) {
        const double a = d + fromMin * 60.0, b = d + toMin * 60.0 + (toMin < fromMin ? 86400.0 : 0.0);
        if (b > t0 && a < t1) out.push_back({a, b});
    }
    return out;
}

}  // namespace dect2
