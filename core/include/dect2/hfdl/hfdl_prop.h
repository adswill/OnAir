// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HFDL propagation map (3.9, docs/SMART5-field-3.9.md item 3). Every HFDL ground station sends a squitter once per 32 s frame on each
// frequency it has in use (ARINC 635). So the squitters this receiver decodes, against the time it listened to each frequency, give the
// share of squitters heard per ground station, frequency and UTC hour: a measured propagation map from the user's own antenna. From it:
//  - which stations are heard at which hours (and how far away they are),
//  - the best frequency now for each station, and the frequency that hears the most stations this hour (where to park the receiver),
//  - "station up": a station heard again after none of its squitters for at least upAfterSec (a band opening towards it).
// Learned over days and kept between runs (save()/load(), plain text). Pure logic, no clock: times are Unix seconds (UTC).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace dect2 {

class HfdlPropagation {
public:
    static constexpr double kSquitterSec = 32;   // one squitter per frame per frequency
    double upAfterSec = 6 * 3600;
    double minListenSec = 600;                   // a rate needs this much listening behind it

    struct Up { int gsId = 0; int khz = 0; double t = 0; double silentSec = 0; };
    struct Best { int khz = 0; double rate = 0; double listenSec = 0; };

    // the receiver listened to these frequencies (kHz) for dtSec, ending at t (call once per tick with the channels in the captured band)
    void listened(double t, const std::vector<int>& khz, double dtSec) {
        if (dtSec <= 0 || dtSec > 3600) return;
        const int h = hourOf(t);
        for (int k : khz) listen_[{k, h}] += dtSec;
    }
    // a squitter (or, with squitter = false, any other ground-to-air burst) from ground station gsId on khz; a station coming up again is appended to `ups`
    void heard(double t, int gsId, int khz, float snrDb, bool squitter, std::vector<Up>* ups = nullptr) {
        if (gsId <= 0 || khz <= 0) return;
        auto& st = last_[gsId];
        if (st > 0 && t - st >= upAfterSec && ups) ups->push_back({gsId, khz, t, t - st});
        if (t > st) st = t;
        auto& rh = recent_[gsId];
        rh.insert((int64_t)std::floor(t / 3600));
        while (!rh.empty() && *rh.begin() < (int64_t)std::floor(t / 3600) - 48) rh.erase(rh.begin());
        if (!squitter) return;
        Cell& c = cells_[{gsId, khz, hourOf(t)}];
        c.heard++;
        c.snrSum += snrDb;
    }
    // share of squitters heard, 0 .. 1; -1 when the frequency was listened to for less than minListenSec at that hour
    double rate(int gsId, int khz, int hour) const {
        auto l = listen_.find({khz, hour});
        if (l == listen_.end() || l->second < minListenSec) return -1;
        auto c = cells_.find({gsId, khz, hour});
        const double heard = c == cells_.end() ? 0 : c->second.heard;
        return std::min(1.0, heard * kSquitterSec / l->second);
    }
    double meanSnr(int gsId, int khz, int hour) const {
        auto c = cells_.find({gsId, khz, hour});
        return c == cells_.end() || !c->second.heard ? 0 : c->second.snrSum / c->second.heard;
    }
    // the frequency with the best rate for a station at an hour (khz 0 when none was heard)
    Best best(int gsId, int hour) const {
        Best b;
        for (const auto& kv : cells_) {
            if (std::get<0>(kv.first) != gsId || std::get<2>(kv.first) != hour) continue;
            const int khz = std::get<1>(kv.first);
            const double r = rate(gsId, khz, hour);
            if (r > b.rate) { b.rate = r; b.khz = khz; b.listenSec = listen_.at({khz, hour}); }
        }
        return b;
    }
    // the frequency on which the most squitters of all stations are heard at that hour (sum of the rates); khz 0 when nothing yet
    Best bestFrequency(int hour) const {
        std::map<int, double> sum;
        for (const auto& kv : cells_) {
            if (std::get<2>(kv.first) != hour) continue;
            const double r = rate(std::get<0>(kv.first), std::get<1>(kv.first), hour);
            if (r > 0) sum[std::get<1>(kv.first)] += r;
        }
        Best b;
        for (const auto& [k, s] : sum) if (s > b.rate) { b.rate = s; b.khz = k; b.listenSec = listen_.at({k, hour}); }
        return b;
    }
    // the UTC hours (0 - 23) at which a station has a rate of at least minRate on some frequency
    std::vector<int> hoursHeard(int gsId, double minRate = 0.05) const {
        std::set<int> h;
        for (const auto& kv : cells_)
            if (std::get<0>(kv.first) == gsId && rate(gsId, std::get<1>(kv.first), std::get<2>(kv.first)) >= minRate) h.insert(std::get<2>(kv.first));
        return {h.begin(), h.end()};
    }
    // heard in the hour slots (Unix hours) of the last 24 h: bit i = i hours ago
    uint32_t last24(int gsId, double now) const {
        auto it = recent_.find(gsId);
        if (it == recent_.end()) return 0;
        const int64_t cur = (int64_t)std::floor(now / 3600);
        uint32_t m = 0;
        for (int64_t s : it->second) if (cur - s >= 0 && cur - s < 24) m |= 1u << (cur - s);
        return m;
    }
    double lastHeard(int gsId) const { auto it = last_.find(gsId); return it == last_.end() ? 0 : it->second; }
    std::vector<int> stations() const { std::vector<int> v; for (const auto& kv : last_) v.push_back(kv.first); return v; }
    static int hourOf(double t) { return (int)(((int64_t)std::floor(t / 3600) % 24 + 24) % 24); }

    std::string save() const {
        std::ostringstream o;
        o << "hfdlprop 1\n";
        for (const auto& [k, s] : listen_) o << "L " << k.first << " " << k.second << " " << s << "\n";
        for (const auto& [k, c] : cells_) o << "C " << std::get<0>(k) << " " << std::get<1>(k) << " " << std::get<2>(k) << " " << c.heard << " " << c.snrSum << "\n";
        for (const auto& [g, t] : last_) o << "T " << g << " " << (int64_t)t << "\n";
        return o.str();
    }
    bool load(const std::string& text) {
        std::istringstream in(text);
        std::string l;
        if (!std::getline(in, l) || l.rfind("hfdlprop 1", 0) != 0) return false;
        while (std::getline(in, l)) {
            std::istringstream ls(l); char k = 0; ls >> k;
            if (k == 'L') { int f, h; double s; if (ls >> f >> h >> s) listen_[{f, h}] = s; }
            else if (k == 'C') { int g, f, h; Cell c; if (ls >> g >> f >> h >> c.heard >> c.snrSum) cells_[{g, f, h}] = c; }
            else if (k == 'T') { int g; int64_t t; if (ls >> g >> t) last_[g] = (double)t; }
        }
        return true;
    }

private:
    struct Cell { uint32_t heard = 0; double snrSum = 0; };
    std::map<std::pair<int, int>, double> listen_;               // (khz, hour) -> seconds
    std::map<std::tuple<int, int, int>, Cell> cells_;            // (gs, khz, hour)
    std::map<int, double> last_;                                 // gs -> last heard
    std::map<int, std::set<int64_t>> recent_;                    // gs -> Unix hours heard, the last two days
};

} // namespace dect2
