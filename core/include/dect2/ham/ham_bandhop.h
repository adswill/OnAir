// Band-hop schedule for an unattended weak-signal receiver (FT8, FT4, WSPR, JS8) on one SDR that can only sit on one band at a time. Pure code.
// Each slot boundary the scheduler picks the band to listen on next: the bands that produced the most spots in the last hour win, but a band that has
// not been visited for exploreS gets a turn anyway (an opening starts on a band that was quiet), and a band is kept for at least minDwellSlots so a
// decode that straddles the change is not cut. Bands with a user "never" flag are skipped. Deterministic: the same history gives the same schedule.
#pragma once
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

class HamBandHop {
public:
    double slotS = 15;           // FT8; 7.5 FT4; 120 WSPR
    double exploreS = 1800;      // every band is visited at least this often
    int minDwellSlots = 4;
    void setBands(const std::vector<std::string>& bands) { for (const auto& b : bands) st_[b]; order_ = bands; }
    void exclude(const std::string& band, bool on) { st_[band].excluded = on; }
    // a decoded spot on a band at time tS
    void spot(const std::string& band, double tS) { st_[band].spots.push_back(tS); }
    // the band to listen on for the slot starting at nowS
    std::string next(double nowS) {
        if (!cur_.empty()) st_[cur_].lastVisit = nowS;       // the band we sit on counts as visited now
        if (!cur_.empty() && nowS - since_ < minDwellSlots * slotS) return cur_;
        std::string best; double bestScore = -1;
        for (const auto& b : order_) {
            B& s = st_[b];
            if (s.excluded) continue;
            while (!s.spots.empty() && s.spots.front() < nowS - 3600) s.spots.erase(s.spots.begin());
            double score = (double)s.spots.size();
            const double idle = nowS - s.lastVisit;
            if (idle >= exploreS) score += 1000.0 + idle;      // overdue: goes first, the longest-neglected first
            else score += 0.5 * idle / exploreS;               // small tie-break toward bands not visited for a while
            if (b == cur_) score -= 0.25;                      // a tie moves on
            if (score > bestScore) { bestScore = score; best = b; }
        }
        if (best.empty()) return cur_;
        if (best != cur_) { cur_ = best; since_ = nowS; }
        st_[cur_].lastVisit = nowS;
        return cur_;
    }
    const std::string& current() const { return cur_; }
private:
    struct B { std::vector<double> spots; double lastVisit = -1e9; bool excluded = false; };
    std::map<std::string, B> st_; std::vector<std::string> order_; std::string cur_; double since_ = 0;
};

} // namespace dect2
