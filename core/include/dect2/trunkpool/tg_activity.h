// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Talkgroup activity for the voice and trunking modes (DMR, P25, NXDN, TETRA, dPMR, EDACS, LTR...): per talkgroup the calls, the airtime and the
// distinct radios over a window, an activity timeline (airtime per time bucket), the busiest talkgroups, and "follow the busiest" with a
// hysteresis so a listener is not thrown between two talkgroups that alternate. Pure logic, no clock of its own (callers pass seconds).
#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <vector>
namespace dect2 {

struct TgRow {
    uint32_t tg = 0;
    int calls = 0;
    double airtimeS = 0;
    int radios = 0;        // distinct sources
    double lastS = 0;      // end of the latest call
};

class TgActivity {
public:
    explicit TgActivity(double keepS = 86400) : keep_(keepS) {}
    // A finished call (or a call so far: endS may grow, call add again only for new calls). Calls with endS < startS are ignored.
    void add(uint32_t tg, uint32_t src, double startS, double endS) {
        if (endS < startS || tg == 0) return;
        calls_.push_back({tg, src, startS, endS});
        const double cut = endS - keep_;
        if (calls_.size() > 4096 || (!calls_.empty() && calls_.front().e < cut))
            calls_.erase(std::remove_if(calls_.begin(), calls_.end(), [&](const C& c) { return c.e < cut; }), calls_.end());
    }
    // airtime counts only the part of a call inside [nowS-windowS, nowS]; sorted by airtime, then calls
    std::vector<TgRow> ranking(double nowS, double windowS) const {
        std::map<uint32_t, TgRow> m;
        std::map<uint32_t, std::set<uint32_t>> rad;
        for (const auto& c : calls_) {
            const double a = std::max(c.s, nowS - windowS), b = std::min(c.e, nowS);
            if (b < a || (b == a && c.e != c.s)) continue;
            if (c.e < nowS - windowS || c.s > nowS) continue;
            TgRow& r = m[c.tg];
            r.tg = c.tg; r.calls++; r.airtimeS += b - a; r.lastS = std::max(r.lastS, c.e);
            rad[c.tg].insert(c.src);
        }
        std::vector<TgRow> out;
        for (auto& kv : m) { kv.second.radios = (int)rad[kv.first].size(); out.push_back(kv.second); }
        std::stable_sort(out.begin(), out.end(), [](const TgRow& a, const TgRow& b) { return a.airtimeS != b.airtimeS ? a.airtimeS > b.airtimeS : a.calls > b.calls; });
        return out;
    }
    // airtime seconds per bucket, oldest first, ending at nowS (a bar chart of "when is this talkgroup used")
    std::vector<double> timeline(uint32_t tg, double nowS, double bucketS, int n) const {
        std::vector<double> v((size_t)std::max(0, n), 0.0);
        if (n <= 0 || bucketS <= 0) return v;
        const double t0 = nowS - bucketS * n;
        for (const auto& c : calls_) {
            if (c.tg != tg || c.e < t0 || c.s > nowS) continue;
            for (int i = 0; i < n; i++) {
                const double a = std::max(c.s, t0 + i * bucketS), b = std::min(c.e, t0 + (i + 1) * bucketS);
                if (b > a) v[(size_t)i] += b - a;
            }
        }
        return v;
    }
    size_t size() const { return calls_.size(); }
private:
    struct C { uint32_t tg, src; double s, e; };
    std::vector<C> calls_;
    double keep_;
};

// Follow the busiest talkgroup. The current one is kept until another has at least `ratio` times its airtime in the window AND at least
// `minAirS` seconds, and the current one has been followed for `minDwellS`. Returns the talkgroup to follow (0: nothing to follow).
class TgFollower {
public:
    double windowS = 120, ratio = 1.5, minAirS = 6, minDwellS = 30;
    uint32_t pick(const TgActivity& a, double nowS) {
        const auto r = a.ranking(nowS, windowS);
        if (r.empty() || r[0].airtimeS < minAirS) return cur_;   // nothing busy: keep what we have
        if (cur_ == 0) { cur_ = r[0].tg; since_ = nowS; return cur_; }
        if (r[0].tg == cur_ || nowS - since_ < minDwellS) return cur_;
        double curAir = 0;
        for (const auto& x : r) if (x.tg == cur_) curAir = x.airtimeS;
        if (r[0].airtimeS >= curAir * ratio) { cur_ = r[0].tg; since_ = nowS; }
        return cur_;
    }
    uint32_t current() const { return cur_; }
private:
    uint32_t cur_ = 0;
    double since_ = 0;
};

}  // namespace dect2
