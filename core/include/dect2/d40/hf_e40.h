// 4.0 HF utility helpers, group E (pure, no GUI). Tests: tests/d40/test_hf_e40.cpp
//  1. BandClock   "best frequency now" learned from this site's own readings: an exponential average of the SNR per (frequency, UTC hour). Works for NCDXF / IBP beacons, time signals,
//                 ALE and HFDL channels alike. No propagation model: until an hour has data, the neighbouring hours (a day-night trend) fill in, and unknowns are reported as unknown.
//  2. fadeStats   fading of a carrier or beacon from its envelope: fade rate (crossings per minute of the median), depth (10 % to 90 % spread in dB) and a verdict (flat / QSB / flutter).
#pragma once
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace dect2::d40 {

class BandClock {
public:
    explicit BandClock(double alpha = 0.3) : alpha_(alpha) {}
    void add(double freqHz, int utcHour, double snrDb) {
        auto& c = m_[{key(freqHz), ((utcHour % 24) + 24) % 24}];
        c.mean = c.n == 0 ? snrDb : c.mean + alpha_ * (snrDb - c.mean); c.n++;
    }
    // expected SNR now; NaN when this hour and both neighbours are unknown. A neighbour-only estimate is marked by usedNeighbours.
    double expected(double freqHz, int utcHour, bool* usedNeighbours = nullptr) const {
        const int h = ((utcHour % 24) + 24) % 24; auto it = m_.find({key(freqHz), h});
        if (usedNeighbours) *usedNeighbours = false;
        if (it != m_.end()) return it->second.mean;
        double s = 0; int n = 0;
        for (int d : {-1, 1}) { auto j = m_.find({key(freqHz), (h + d + 24) % 24}); if (j != m_.end()) { s += j->second.mean; n++; } }
        if (!n) return std::nan("");
        if (usedNeighbours) *usedNeighbours = true; return s / n;
    }
    struct Pick { double freqHz = 0, snrDb = 0; bool ok = false; };
    Pick best(const std::vector<double>& freqs, int utcHour, double minSnrDb = 0) const {
        Pick p; for (double f : freqs) { const double e = expected(f, utcHour); if (!std::isnan(e) && e >= minSnrDb && (!p.ok || e > p.snrDb)) { p.freqHz = f; p.snrDb = e; p.ok = true; } }
        return p;
    }
    // the next hour at which a different frequency becomes the best (0 = none in the next 24 hours)
    int hoursToChange(const std::vector<double>& freqs, int utcHour) const {
        const Pick a = best(freqs, utcHour); if (!a.ok) return 0;
        for (int d = 1; d <= 24; d++) { const Pick b = best(freqs, utcHour + d); if (b.ok && b.freqHz != a.freqHz) return d; }
        return 0;
    }
private:
    static long long key(double f) { return (long long)std::llround(f / 100.0); }   // 100 Hz bins
    struct Cell { double mean = 0; int n = 0; };
    std::map<std::pair<long long, int>, Cell> m_; double alpha_;
};

struct FadeStats { double ratePerMin = 0, depthDb = 0; std::string verdict; bool ok = false; };
// env: envelope power in dB, one value per sample at fsHz (a few Hz is plenty). Needs about 30 s.
inline FadeStats fadeStats(const std::vector<float>& env, double fsHz) {
    FadeStats r; const size_t n = env.size(); if (n < 32 || !(fsHz > 0)) return r;
    std::vector<float> s = env; std::sort(s.begin(), s.end());
    const double med = s[n / 2]; r.depthDb = s[(size_t)(0.9 * (n - 1))] - s[(size_t)(0.1 * (n - 1))];
    // hysteresis around the median: ignore wiggles smaller than 15 % of the depth
    const double h = 0.15 * r.depthDb; int cross = 0; int st = 0;
    for (float v : env) { const int ns = v > med + h ? 1 : v < med - h ? -1 : st; if (st != 0 && ns != st) cross++; st = ns; }
    const double minutes = (double)n / fsHz / 60.0; r.ratePerMin = cross / 2.0 / minutes; r.ok = true;
    r.verdict = r.depthDb < 3 ? "flat" : r.ratePerMin > 120 ? "flutter" : "QSB";
    return r;
}

}  // namespace dect2::d40
