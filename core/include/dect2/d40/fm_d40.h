// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 4.0 FM / RDS helpers (pure rules, no GUI). Tests: tests/d40/test_fm_d40.cpp
//  1. nameStations   turns the raw hits of a span scan (frequency, level, PI, many PS reads) into one named station list: the PS is the majority of the clean reads
//                    (8 characters, printable, not mid-update), a PI heard on two frequencies within 300 kHz is one station.
//  2. DxOpening      tropo / Es alarm: compares what is heard now with the stations this site normally receives; raises when several foreign-country PIs (country code = high
//                    nibble of the PI) that are NOT in the local baseline show up with a usable level, each confirmed by repeated reads.
//  3. SfnFollower    programme following inside one network: same PI on several frequencies; picks the strongest, but only switches when the other is better by a margin
//                    for several readings in a row, and never to a frequency whose PS does not match.
#pragma once
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dect2::d40 {

struct FmHit {
    double mhz = 0;
    double snrDb = 0;
    int pi = 0;                     // 0 = not decoded
    std::vector<std::string> psReads;
};
struct FmStation { double mhz = 0; double snrDb = 0; int pi = 0; std::string name; std::vector<double> alsoMhz; };

inline bool psClean(const std::string& s) {
    if (s.size() != 8) return false;
    for (unsigned char c : s) if (c < 0x20 || c > 0x7e) return false;
    return true;
}
inline std::string psMajority(const std::vector<std::string>& reads, int minVotes = 2) {
    std::map<std::string, int> v;
    for (const auto& r : reads) if (psClean(r)) v[r]++;
    std::string best; int bn = 0;
    for (const auto& q : v) if (q.second > bn) { bn = q.second; best = q.first; }
    if (bn < minVotes) return std::string();
    while (!best.empty() && best.back() == ' ') best.pop_back();
    return best;
}

inline std::vector<FmStation> nameStations(const std::vector<FmHit>& hits, double minSnrDb = 6) {
    std::vector<FmStation> out;
    std::vector<FmHit> h;
    for (const auto& x : hits) if (x.snrDb >= minSnrDb) h.push_back(x);
    std::sort(h.begin(), h.end(), [](const FmHit& a, const FmHit& b) { return a.snrDb > b.snrDb; });   // strongest first becomes the main entry
    for (const auto& x : h) {
        bool merged = false;
        if (x.pi > 0)
            for (auto& s : out)
                if (s.pi == x.pi && std::fabs(s.mhz - x.mhz) <= 0.3 && std::fabs(s.mhz - x.mhz) > 1e-6) { s.alsoMhz.push_back(x.mhz); merged = true; break; }
        if (merged) continue;
        FmStation s; s.mhz = x.mhz; s.snrDb = x.snrDb; s.pi = x.pi; s.name = psMajority(x.psReads);
        out.push_back(s);
    }
    std::sort(out.begin(), out.end(), [](const FmStation& a, const FmStation& b) { return a.mhz < b.mhz; });
    return out;
}

class DxOpening {
public:
    int homeCountry = -1;      // PI high nibble of the home country, -1 = learn: the most common one in the baseline
    int minNew = 3;            // new foreign stations needed
    int minReads = 2;          // PI reads that must agree for a station to count
    double minSnrDb = 8;
    // learning phase: call with what a normal scan hears
    void learn(int pi, double mhz) { base_.insert(key(pi, mhz)); cc_[(pi >> 12) & 15]++; }
    // one PI read on a frequency now
    void observe(int pi, double mhz, double snrDb) {
        if (pi <= 0) return;
        Now& n = now_[key(pi, mhz)];
        n.reads++; n.snr = std::max(n.snr, snrDb); n.pi = pi; n.mhz = mhz;
    }
    struct Alarm { bool on = false; std::vector<int> pis; std::string text; };
    Alarm check() const {
        Alarm a;
        int home = homeCountry;
        if (home < 0) { int bn = 0; for (const auto& q : cc_) if (q.second > bn) { bn = q.second; home = q.first; } }
        for (const auto& q : now_) {
            const Now& n = q.second;
            if (n.reads < minReads || n.snr < minSnrDb || base_.count(q.first)) continue;
            if (home >= 0 && ((n.pi >> 12) & 15) == home) continue;   // a new home station is not a DX opening
            if (baseHasPi(n.pi)) continue;
            a.pis.push_back(n.pi);
        }
        std::sort(a.pis.begin(), a.pis.end());
        a.pis.erase(std::unique(a.pis.begin(), a.pis.end()), a.pis.end());
        if ((int)a.pis.size() >= minNew) { a.on = true; a.text = std::to_string(a.pis.size()) + " foreign stations that are not heard here normally: tropo or sporadic-E opening"; }
        return a;
    }
    void clearNow() { now_.clear(); }
private:
    struct Now { int reads = 0; double snr = -99; int pi = 0; double mhz = 0; };
    static long key(int pi, double mhz) { return (long)pi * 100000L + std::lround(mhz * 10.0); }
    bool baseHasPi(int pi) const { for (long k : base_) if (k / 100000L == pi) return true; return false; }
    std::set<long> base_;
    std::map<int, int> cc_;
    std::map<long, Now> now_;
};

class SfnFollower {
public:
    double marginDb = 4;       // the other frequency must be this much better
    int holdReads = 3;         // ... for this many readings in a row
    struct Cand { double mhz = 0; double snrDb = 0; int pi = 0; std::string ps; };
    // cur: the frequency now tuned; returns the frequency to tune (cur when staying)
    double update(double cur, int curPi, const std::string& curPs, const std::vector<Cand>& cands) {
        const Cand* me = nullptr; const Cand* best = nullptr;
        for (const auto& c : cands) {
            if (std::fabs(c.mhz - cur) < 1e-6) me = &c;
            if (c.pi != curPi) continue;
            if (!curPs.empty() && !c.ps.empty() && c.ps != curPs) continue;   // same PI, other programme (regional split)
            if (std::fabs(c.mhz - cur) < 1e-6) continue;
            if (!best || c.snrDb > best->snrDb) best = &c;
        }
        const double mine = me ? me->snrDb : -99;
        if (best && best->snrDb >= mine + marginDb) {
            if (std::fabs(best->mhz - cand_) < 1e-6) streak_++; else { cand_ = best->mhz; streak_ = 1; }
            if (streak_ >= holdReads) { streak_ = 0; cand_ = 0; return best->mhz; }
        } else { streak_ = 0; cand_ = 0; }
        return cur;
    }
private:
    double cand_ = 0; int streak_ = 0;
};

}  // namespace dect2::d40
