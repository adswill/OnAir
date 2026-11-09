#include "dect2/trunkpool/cc_health.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dect2::tpool {

ControlHealth::Bucket& ControlHealth::bucket(Ch& c, double now) {
    const int64_t m = (int64_t)std::floor(now / 60.0);
    if (c.b.empty() || c.b.back().minute != m) {
        c.b.push_back({m, 0, 0, 0});
        const int64_t keep = m - (int64_t)cfg_.windowMin;
        while (!c.b.empty() && c.b.front().minute <= keep) c.b.erase(c.b.begin());
    }
    return c.b.back();
}

void ControlHealth::onGood(double hz, double now) { Ch& c = ch_[key(hz)]; bucket(c, now).good++; c.lastGood = now; }
void ControlHealth::onBad(double hz, double now) { Ch& c = ch_[key(hz)]; bucket(c, now).bad++; }
void ControlHealth::onGrant(double hz, double now) { bucket(ch_[key(hz)], now).grants++; }

CcScore ControlHealth::score(double hz, double now, double win) const {
    if (win <= 0) win = cfg_.windowMin;
    CcScore s; s.hz = hz;
    auto it = ch_.find(key(hz));
    if (it == ch_.end()) return s;
    const Ch& c = it->second;
    int good = 0, bad = 0, grants = 0;
    const int64_t m = (int64_t)std::floor(now / 60.0);
    int64_t first = m;
    for (const Bucket& b : c.b) if (b.minute > m - (int64_t)win) { good += b.good; bad += b.bad; grants += b.grants; first = std::min(first, b.minute); }
    s.minutes = (int)(m - first + 1);
    const double mins = std::max(1.0, (double)s.minutes);
    s.errorRate = good + bad > 0 ? (double)bad / (good + bad) : 1.0;
    s.msgPerMin = good / mins; s.grantsPerMin = grants / mins;
    s.ageSec = now - c.lastGood;
    const double act = std::min(1.0, s.msgPerMin / 30.0);           // about one message every two seconds is a living control channel
    const double rec = s.ageSec <= 10 ? 1.0 : std::exp(-(s.ageSec - 10) / 20.0);
    s.score = (1 - s.errorRate) * (1 - s.errorRate) * (0.6 + 0.4 * act) * rec;
    return s;
}

std::vector<CcScore> ControlHealth::all(double now) const {
    std::vector<CcScore> v;
    for (const auto& kv : ch_) v.push_back(score((double)kv.first, now));
    std::sort(v.begin(), v.end(), [](const CcScore& a, const CcScore& b) { return a.score > b.score; });
    return v;
}

double ControlHealth::advise(double current, double now) {
    auto it = ch_.find(key(current));
    const CcScore cur = score(current, now, cfg_.recentMin);
    const bool bad = it == ch_.end() || cur.score < cfg_.leaveBelow;
    if (it != ch_.end()) {
        Ch& c = it->second;
        if (bad) { if (c.belowSince < 0) c.belowSince = now; } else c.belowSince = -1;
    }
    if (!bad || now - lastMove_ < cfg_.minDwellSec) return 0;
    const double since = it != ch_.end() && it->second.belowSince >= 0 ? it->second.belowSince : now - cfg_.badForSec;
    if (now - since < cfg_.badForSec) return 0;
    double best = 0, bs = 0;
    for (const auto& kv : ch_) {
        const double hz = (double)kv.first;
        if (std::fabs(hz - current) < 1) continue;
        const CcScore s = score(hz, now, cfg_.recentMin);
        if (s.minutes < cfg_.minDataMin || s.ageSec > 600) continue;   // never visited long enough, or not heard for ten minutes: not a target
        if (s.score > bs) { bs = s.score; best = hz; }
    }
    return best > 0 && bs >= cur.score + cfg_.betterBy ? best : 0;
}

void ControlHealth::learnSite(int sysId, int rfss, int site, double ccHz, double now) {
    if (ccHz <= 0) return;
    for (Site& s : sites_) if (s.sysId == sysId && s.rfss == rfss && s.site == site) { s.ccHz = ccHz; s.lastSec = now; s.seen++; return; }
    sites_.push_back({sysId, rfss, site, ccHz, now, 1});
}

std::string ControlHealth::exportCsv() const {
    std::string o = "system,rfss,site,control_hz,seen\n";
    char b[96];
    for (const Site& s : sites_) { snprintf(b, sizeof b, "%d,%d,%d,%.0f,%d\n", s.sysId, s.rfss, s.site, s.ccHz, s.seen); o += b; }
    return o;
}

int ControlHealth::importCsv(const std::string& text) {
    std::istringstream in(text);
    std::string line; int n = 0;
    while (std::getline(in, line)) {
        int sy, rf, si, seen = 1; double hz;
        if (sscanf(line.c_str(), "%d,%d,%d,%lf,%d", &sy, &rf, &si, &hz, &seen) >= 4) { learnSite(sy, rf, si, hz, 0); n++; }
    }
    return n;
}

} // namespace dect2::tpool
