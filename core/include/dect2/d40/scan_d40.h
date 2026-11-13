// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 4.0 explorer / scanner helpers (pure, no GUI). Tests: tests/d40/test_scan_d40.cpp
//  1. classifyHit      names the kind of signal behind every hit of a sweep from what a sweep can measure over a few passes: width, how often it was there (duty), level
//                      swing, whether the width is a known channel raster, and the band it sits in. Ranked list with a reason; "unknown" is allowed and honest.
//  2. ActivityMap      persistent band activity over days: per frequency cell and hour of the day, how many sweeps saw something and how many sweeps were made. Saved as text and
//                      loaded again, so it grows over days; gives the busy hours of a cell and a heat row for an hour.
//  3. NewSignalWatch   alert when a signal appears where the map says that hour in that cell is normally empty (seen in under 2 % of at least 20 sweeps), after it was
//                      present in `confirm` consecutive sweeps (so one spur does not alert), and not again for the same cell until it was gone for `rearm` sweeps.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace dect2::d40 {

struct HitFeatures { double centreHz = 0, bwHz = 0; double duty = 1; double levelStdDb = 0; double peakToMeanDb = 0; bool carrierLine = false; };
struct HitClass { std::string what; double confidence = 0; std::string why; };

inline std::vector<HitClass> classifyHit(const HitFeatures& f) {
    std::vector<HitClass> out;
    const double mhz = f.centreHz / 1e6;
    auto add = [&](const char* w, double c, const std::string& why) { out.push_back({w, std::min(0.9, c), why}); };
    if (f.bwHz < 3e3 && f.duty > 0.9 && f.levelStdDb < 2) add("unmodulated carrier / beacon / spur", 0.7, "narrow, always on, steady level");
    if (f.bwHz < 3e3 && f.duty <= 0.9) add("keyed carrier (CW / beacon)", 0.5, "narrow and intermittent");
    if (mhz >= 87.5 && mhz <= 108.0 && f.bwHz > 120e3 && f.bwHz < 260e3) add("FM broadcast", f.duty > 0.9 ? 0.85 : 0.6, "wide, in the FM band, continuous");
    if (mhz >= 118.0 && mhz <= 137.0 && f.bwHz < 15e3) add("airband AM voice", f.duty < 0.6 ? 0.8 : 0.5, "narrow, in the airband, intermittent");
    if (f.bwHz >= 6e3 && f.bwHz <= 16e3 && f.carrierLine && f.duty > 0.9) add("AM broadcast", 0.65, "10 kHz wide with a carrier line, continuous");
    if (f.bwHz >= 6e3 && f.bwHz <= 25e3 && f.duty < 0.7 && f.levelStdDb > 3) add("narrow FM voice", 0.5, "12.5 / 25 kHz channel, keyed on and off");
    if (f.bwHz >= 5e3 && f.bwHz <= 25e3 && f.duty < 0.5 && f.peakToMeanDb < 3) add("digital voice / data burst", 0.4, "channel-wide flat top, short bursts");
    if (f.bwHz > 1e6 && f.duty > 0.9 && f.peakToMeanDb < 4) add("wideband digital (OFDM / TV / LTE)", 0.7, "over 1 MHz, flat, continuous");
    if (f.bwHz > 1e6 && f.duty < 0.5) add("wideband burst (radar / frequency hopper / Wi-Fi)", 0.5, "wide and intermittent");
    if (f.bwHz > 25e3 && f.bwHz <= 1e6 && f.duty > 0.9 && f.peakToMeanDb < 4) add("continuous digital (DAB / TETRA / DVB narrow / ISM)", 0.45, "25 kHz to 1 MHz, flat, continuous");
    if (f.bwHz <= 25e3 && f.duty < 0.1) add("rare burst (sensor / pager / ADS-B-like)", 0.45, "present in under 10 % of passes");
    if (out.empty()) out.push_back({"unknown", 0.1, "no rule fits: record it and look at the waterfall"});
    std::stable_sort(out.begin(), out.end(), [](const HitClass& a, const HitClass& b) { return a.confidence > b.confidence; });
    return out;
}

class ActivityMap {
public:
    double cellHz = 25e3;
    // one sweep: the hit centre frequencies it found (Hz) and the cells it covered (lowHz..highHz), at local hour 0-23
    void addSweep(int hour, double lowHz, double highHz, const std::vector<double>& hitsHz) {
        hour = ((hour % 24) + 24) % 24;
        const long c0 = cell(lowHz), c1 = cell(highHz);
        for (long c = c0; c <= c1; c++) c_[c].sweeps[hour]++;
        std::map<long, bool> seen;
        for (double h : hitsHz) { const long c = cell(h); if (c >= c0 && c <= c1) seen[c] = true; }
        for (auto& s : seen) c_[s.first].seen[hour]++;
    }
    double occupancy(double hz, int hour) const {
        auto i = c_.find(cell(hz)); if (i == c_.end() || i->second.sweeps[hour] == 0) return -1;
        return (double)i->second.seen[hour] / i->second.sweeps[hour];
    }
    int sweepsAt(double hz, int hour) const { auto i = c_.find(cell(hz)); return i == c_.end() ? 0 : i->second.sweeps[hour]; }
    int sweepsAny(double hz) const { auto i = c_.find(cell(hz)); if (i == c_.end()) return 0; int s = 0; for (int h = 0; h < 24; h++) s += i->second.sweeps[h]; return s; }
    int seenAny(double hz) const { auto i = c_.find(cell(hz)); if (i == c_.end()) return 0; int s = 0; for (int h = 0; h < 24; h++) s += i->second.seen[h]; return s; }
    std::vector<int> busyHours(double hz, double minOcc = 0.3) const { std::vector<int> r; for (int h = 0; h < 24; h++) { const double o = occupancy(hz, h); if (o >= minOcc) r.push_back(h); } return r; }
    // one text line per cell: "cell sweeps0..23 seen0..23"; load() merges into what is there (days add up)
    std::string save() const {
        std::ostringstream o; o << "ACTMAP1 " << (long)cellHz << "\n";
        for (const auto& q : c_) { o << q.first; for (int h = 0; h < 24; h++) o << ' ' << q.second.sweeps[h]; for (int h = 0; h < 24; h++) o << ' ' << q.second.seen[h]; o << "\n"; }
        return o.str();
    }
    bool load(const std::string& text) {
        std::istringstream in(text); std::string tag; long cz = 0;
        if (!(in >> tag >> cz) || tag != "ACTMAP1" || cz <= 0) return false;
        if (c_.empty()) cellHz = (double)cz; else if ((long)cellHz != cz) return false;
        long c; 
        while (in >> c) {
            Cell x; bool ok = true;
            for (int h = 0; h < 24; h++) if (!(in >> x.sweeps[h])) ok = false;
            for (int h = 0; h < 24; h++) if (!(in >> x.seen[h])) ok = false;
            if (!ok) return false;
            Cell& d = c_[c]; for (int h = 0; h < 24; h++) { d.sweeps[h] += x.sweeps[h]; d.seen[h] += x.seen[h]; }
        }
        return true;
    }
    size_t cells() const { return c_.size(); }
private:
    struct Cell { int sweeps[24] = {0}; int seen[24] = {0}; };
    long cell(double hz) const { return (long)std::floor(hz / cellHz); }
    std::map<long, Cell> c_;
};

class NewSignalWatch {
public:
    int confirm = 2, rearm = 10, minBaselineSweeps = 20; double maxBaselineOcc = 0.02;
    struct Alert { double hz = 0; int hour = 0; std::string text; };
    // call after each sweep, BEFORE adding it to the map: map is the baseline
    std::vector<Alert> check(const ActivityMap& map, int hour, const std::vector<double>& hitsHz) {
        std::vector<Alert> out;
        std::map<long, double> now;
        for (double h : hitsHz) now[key(map, h)] = h;
        for (auto& n : now) {
            St& s = st_[n.first]; s.run++; s.gone = 0;
            if (s.run >= confirm && !s.alerted) {
                const int sw = map.sweepsAt(n.second, hour);
                const double occ = map.occupancy(n.second, hour);
                if (sw >= minBaselineSweeps && occ >= 0 && occ <= maxBaselineOcc) {
                    s.alerted = true;
                    out.push_back({n.second, hour, "new signal at " + std::to_string(n.second / 1e6) + " MHz: seen in " + std::to_string((int)std::lround(occ * 100)) + " % of " + std::to_string(sw) + " earlier sweeps at this hour"});
                }
            }
        }
        for (auto& q : st_) if (!now.count(q.first)) { q.second.run = 0; if (++q.second.gone >= rearm) q.second.alerted = false; }
        return out;
    }
private:
    struct St { int run = 0, gone = 0; bool alerted = false; };
    static long key(const ActivityMap& m, double hz) { return (long)std::floor(hz / m.cellHz); }
    std::map<long, St> st_;
};

}  // namespace dect2::d40
