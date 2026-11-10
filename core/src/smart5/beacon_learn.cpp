// CubeSat beacon interval learner (see beacon_learn.h).
#include "dect2/smart5/beacon_learn.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {
namespace smart5 {

void BeaconLearn::learn(S& s) {
    // intervals within one pass (gaps over 20 min are between passes); the period is the shortest common step:
    // take the smallest interval as the first guess, count each interval as a whole number of periods, refine by least squares
    std::vector<double> d;
    for (size_t i = 1; i < s.t.size(); i++) { const double x = s.t[i] - s.t[i - 1]; if (x > 0.5 && x < 1200) d.push_back(x); }
    if (d.size() < 3) return;
    std::vector<double> sd(d);
    std::sort(sd.begin(), sd.end());
    // smallest interval that is not an outlier: the 10th percentile
    double p = sd[sd.size() / 10];
    for (int it = 0; it < 3; it++) {
        double num = 0, den = 0;
        for (double x : d) { const double k = std::round(x / p); if (k >= 1 && std::fabs(x - k * p) < 0.25 * p) { num += x * k; den += k * k; } }
        if (den > 0) p = num / den;
    }
    double var = 0; int n = 0;
    for (double x : d) { const double k = std::round(x / p); if (k >= 1) { const double e = x - k * p; if (std::fabs(e) < 0.25 * p) { var += e * e; n++; } } }
    s.st.periodSec = p;
    s.st.jitterSec = n ? std::sqrt(var / n) : 0;
}

void BeaconLearn::beacon(int norad, double t) {
    S& s = s_[norad];
    s.t.push_back(t);
    while (s.t.size() > 200) s.t.pop_front();
    s.st.heard++;
    s.st.last = t;
    learn(s);
    s.st.nextDue = s.st.periodSec > 0 ? t + s.st.periodSec : 0;
    s.st.overdue = false;
}

void BeaconLearn::tick(int norad, double now, bool up) {
    auto it = s_.find(norad);
    if (it == s_.end()) return;
    S& s = it->second;
    BeaconState& b = s.st;
    if (b.periodSec <= 0) return;
    const double slack = std::max(1.0, 4 * b.jitterSec) + 0.1 * b.periodSec;
    if (!up) { b.overdue = false; while (b.nextDue + slack < now) b.nextDue += b.periodSec; return; }
    while (b.nextDue + slack < now) {
        if (b.nextDue > s.lastJudged) { b.missedWhileUp++; s.lastJudged = b.nextDue; }
        b.overdue = true;
        b.nextDue += b.periodSec;
    }
}

BeaconState BeaconLearn::state(int norad) const { auto it = s_.find(norad); return it == s_.end() ? BeaconState{} : it->second.st; }

std::string BeaconLearn::text(int norad, const std::string& name, double now) const {
    const BeaconState b = state(norad);
    char x[200];
    if (b.periodSec <= 0) snprintf(x, sizeof x, "%s: learning the beacon period (%d heard)", name.c_str(), b.heard);
    else if (b.overdue) snprintf(x, sizeof x, "%s: beacon overdue (every %.1f s, %d missed while up)", name.c_str(), b.periodSec, b.missedWhileUp);
    else snprintf(x, sizeof x, "%s: beacon every %.1f s (+-%.1f), next in %.0f s", name.c_str(), b.periodSec, b.jitterSec, std::max(0.0, b.nextDue - now));
    return x;
}

} // namespace smart5
} // namespace dect2
