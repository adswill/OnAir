// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 4.0 DAB helpers (pure, no GUI). Tests: tests/d40/test_dab_d40.cpp
//  1. ServiceRanker   the census merge shows a service once at its best SNR of the moment; this keeps a history per service (mean SNR, share of scans where it decoded, spread) and
//                     ranks by sustained quality: SNR pulled down by the lock failures and by instability, so the best service to put on a preset is the one that stays good.
//  2. TiiTable        transmitter identification: the user loads a transmitter list (CSV: main,sub,name,lat,lon[,kW]); heard (main,sub) pairs become named transmitters with distance and
//                     bearing from the receiver, nearest first, and the pairs not in the table are listed as unknown (a new or foreign transmitter).
//  3. BlobCollector   slideshow / logo / album-art collector: stores every distinct picture (by content hash) per service once, keeps the newest N, tells whether a received picture is new.
//                     Also used for HD Radio logos and album art (hd_d40 shares it).
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

struct RankedService { uint32_t sid = 0; std::string label; double score = 0; double meanSnr = 0; double lockShare = 0; double spreadDb = 0; int scans = 0; };
class ServiceRanker {
public:
    // one scan: snrDb for a service that decoded, or lockedOk=false for a scan where the ensemble was in range but the service did not decode
    void add(uint32_t sid, const std::string& label, bool locked, double snrDb) {
        Acc& a = m_[sid];
        a.label = label.empty() ? a.label : label;
        a.scans++;
        if (locked) { a.locked++; a.sum += snrDb; a.sum2 += snrDb * snrDb; }
    }
    std::vector<RankedService> ranking(int minScans = 3) const {
        std::vector<RankedService> out;
        for (const auto& q : m_) {
            const Acc& a = q.second;
            if (a.scans < minScans || a.locked == 0) continue;
            RankedService r; r.sid = q.first; r.label = a.label; r.scans = a.scans;
            r.meanSnr = a.sum / a.locked;
            r.spreadDb = std::sqrt(std::max(0.0, a.sum2 / a.locked - r.meanSnr * r.meanSnr));
            r.lockShare = (double)a.locked / a.scans;
            r.score = r.meanSnr - 1.5 * r.spreadDb - 20.0 * (1.0 - r.lockShare);
            out.push_back(r);
        }
        std::sort(out.begin(), out.end(), [](const RankedService& a, const RankedService& b) { return a.score > b.score; });
        return out;
    }
private:
    struct Acc { std::string label; int scans = 0, locked = 0; double sum = 0, sum2 = 0; };
    std::map<uint32_t, Acc> m_;
};

struct TiiRow { int mainId = 0, subId = 0; std::string name; double lat = 0, lon = 0, kw = 0; };
struct TiiHeard { int mainId = 0, subId = 0; bool known = false; std::string name; double distKm = 0, bearingDeg = 0, kw = 0; };
class TiiTable {
public:
    int load(const std::string& csv) {
        std::istringstream in(csv); std::string line; int ok = 0;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::vector<std::string> f; std::string c; std::istringstream ls(line);
            while (std::getline(ls, c, ',')) f.push_back(c);
            if (f.size() < 5) continue;
            TiiRow r; r.mainId = std::atoi(f[0].c_str()); r.subId = std::atoi(f[1].c_str()); r.name = f[2];
            char* e = nullptr; r.lat = std::strtod(f[3].c_str(), &e); if (e == f[3].c_str()) continue;
            r.lon = std::strtod(f[4].c_str(), &e); if (e == f[4].c_str()) continue;
            if (f.size() > 5) r.kw = std::atof(f[5].c_str());
            if (r.mainId < 0 || r.mainId > 69 || r.subId < 1 || r.subId > 23 || std::fabs(r.lat) > 90 || std::fabs(r.lon) > 180) continue;   // TII: main 0-69, sub 1-23
            rows_.push_back(r); ok++;
        }
        return ok;
    }
    std::vector<TiiHeard> identify(const std::vector<std::pair<int, int>>& heard, double myLat, double myLon) const {
        std::vector<TiiHeard> out;
        for (const auto& h : heard) {
            TiiHeard t; t.mainId = h.first; t.subId = h.second;
            for (const auto& r : rows_) if (r.mainId == h.first && r.subId == h.second) {
                t.known = true; t.name = r.name; t.kw = r.kw;
                t.distKm = distKm(myLat, myLon, r.lat, r.lon); t.bearingDeg = bearing(myLat, myLon, r.lat, r.lon);
                break;
            }
            out.push_back(t);
        }
        std::stable_sort(out.begin(), out.end(), [](const TiiHeard& a, const TiiHeard& b) { if (a.known != b.known) return a.known; return a.distKm < b.distKm; });
        return out;
    }
    static double distKm(double la1, double lo1, double la2, double lo2) {
        const double r = M_PI / 180, dla = (la2 - la1) * r, dlo = (lo2 - lo1) * r;
        const double a = std::sin(dla / 2) * std::sin(dla / 2) + std::cos(la1 * r) * std::cos(la2 * r) * std::sin(dlo / 2) * std::sin(dlo / 2);
        return 12742.0 * std::asin(std::min(1.0, std::sqrt(a)));
    }
    static double bearing(double la1, double lo1, double la2, double lo2) {
        const double r = M_PI / 180, dlo = (lo2 - lo1) * r;
        const double y = std::sin(dlo) * std::cos(la2 * r), x = std::cos(la1 * r) * std::sin(la2 * r) - std::sin(la1 * r) * std::cos(la2 * r) * std::cos(dlo);
        double b = std::atan2(y, x) / r; if (b < 0) b += 360; return b;
    }
private:
    std::vector<TiiRow> rows_;
};

inline uint64_t fnv1a64(const uint8_t* p, size_t n) { uint64_t h = 1469598103934665603ULL; for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; } return h; }

struct Blob { uint64_t hash = 0; std::string kind; std::string mime; std::vector<uint8_t> data; double firstSeenSec = 0; int seen = 1; };
class BlobCollector {
public:
    size_t maxPerKey = 30;
    // key: service (SId / station); returns true when the picture is new for that key
    bool add(const std::string& key, const std::string& kind, const std::string& mime, const std::vector<uint8_t>& data, double nowSec) {
        if (data.size() < 16) return false;
        const uint64_t h = fnv1a64(data.data(), data.size()) ^ (uint64_t)data.size();
        auto& v = m_[key];
        for (auto& b : v) if (b.hash == h) { b.seen++; return false; }
        Blob b; b.hash = h; b.kind = kind; b.mime = mime; b.data = data; b.firstSeenSec = nowSec;
        v.push_back(std::move(b));
        if (v.size() > maxPerKey) v.erase(v.begin());   // oldest out
        return true;
    }
    const std::vector<Blob>& get(const std::string& key) const { static const std::vector<Blob> none; auto i = m_.find(key); return i == m_.end() ? none : i->second; }
    size_t keys() const { return m_.size(); }
private:
    std::map<std::string, std::vector<Blob>> m_;
};

}  // namespace dect2::d40
