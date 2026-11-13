// Radiosonde launch clock: learns when each launch site sends its sondes (the UTC time of day of the launches this station heard, kept per site)
// and predicts the next launch, so the receiver can be tuned to the site's frequency a few minutes ahead. Most sites launch at 00 / 12 UTC (some
// 06 / 18, some every few hours) at the same minute every day, so a launch time of day clusters tightly. Pure logic; the caller keeps the history.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
namespace dect2 {

struct LaunchPrediction {
    bool valid = false;
    int64_t nextUtc = 0;      // expected launch (unix seconds)
    int spreadMin = 0;        // typical scatter of that time of day, minutes
    int samples = 0;          // launches the time of day is based on
    int64_t listenFromUtc = 0; // start tuning here (launch minus lead time)
};

// launches: unix times of the launches heard at one site. Clusters the times of day (a cluster = launches within clusterMin of each other);
// every cluster seen on at least minDays distinct days is a regular slot. Returns the next slot after nowUtc.
inline LaunchPrediction predictNextLaunch(const std::vector<int64_t>& launches, int64_t nowUtc, int minDays = 3, int clusterMin = 40, int leadMin = 15) {
    LaunchPrediction p;
    struct S { double tod; int64_t day; };
    std::vector<S> s;
    for (int64_t t : launches) { if (t <= 0) continue; const int64_t day = t / 86400; s.push_back({double(t - day * 86400) / 60.0, day}); }
    std::sort(s.begin(), s.end(), [](const S& a, const S& b) { return a.tod < b.tod; });
    // clusters on the 1440 min circle: split at gaps larger than clusterMin
    std::vector<std::vector<S>> cl;
    for (const auto& x : s) {
        if (cl.empty() || x.tod - cl.back().back().tod > clusterMin) cl.push_back({x}); else cl.back().push_back(x);
    }
    if (cl.size() > 1 && (cl.front().front().tod + 1440 - cl.back().back().tod) <= clusterMin) {   // wrap around midnight
        for (auto& x : cl.front()) x.tod += 1440;
        cl.back().insert(cl.back().end(), cl.front().begin(), cl.front().end());
        cl.erase(cl.begin());
    }
    int64_t best = 0;
    for (const auto& c : cl) {
        std::vector<int64_t> days;
        for (const auto& x : c) days.push_back(x.day);
        std::sort(days.begin(), days.end());
        days.erase(std::unique(days.begin(), days.end()), days.end());
        if ((int)days.size() < minDays) continue;
        std::vector<double> tods;
        for (const auto& x : c) tods.push_back(x.tod);
        std::sort(tods.begin(), tods.end());
        const double med = tods[tods.size() / 2];
        const double lo = tods.front(), hi = tods.back();
        const int64_t day0 = nowUtc / 86400;
        for (int64_t d = day0 - 1; d <= day0 + 1; d++) {
            const int64_t t = d * 86400 + (int64_t)std::llround(med * 60.0);
            if (t > nowUtc && (best == 0 || t < best)) {
                best = t; p.spreadMin = (int)std::llround(hi - lo); p.samples = (int)tods.size();
            }
        }
    }
    if (best) { p.valid = true; p.nextUtc = best; p.listenFromUtc = best - (int64_t)leadMin * 60 - (int64_t)p.spreadMin * 30; }
    return p;
}

// true when the receiver should already be tuned to the site: from listenFromUtc until the launch plus holdMin
inline bool launchWindowOpen(const LaunchPrediction& p, int64_t nowUtc, int holdMin = 20) {
    return p.valid && nowUtc >= p.listenFromUtc && nowUtc <= p.nextUtc + (int64_t)holdMin * 60;
}

}  // namespace dect2
