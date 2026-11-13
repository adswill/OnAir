// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// APRS network health (docs/SMART5-ham-3.9.md, item 4), from the packets this receiver decodes:
//  over-beaconing  - a fixed station more often than every 10 minutes, a moving one more often than every minute, over at least 6 beacons
//  long paths      - WIDEn-N with n > 2, RELAY, TRACE, plain WIDE (the paths most regions ask people not to use any more)
//  duplicate flood - one packet heard through more than 4 different digipeaters within 30 s
//  silent digi     - a digipeater that relayed regularly and has not been heard for 6 of its usual gaps (and at least 30 minutes)
// Paths are AX.25 path texts as packet_ax25.h prints them: "LY2X*,WIDE2-1" (the star marks the last digipeater that repeated it).
#pragma once
#include "dect2/insight/insight.h"
#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace dect2::insight {

struct AprsHealthLimits {
    double fixedMinSec = 600, movingMinSec = 60;
    int beaconsNeeded = 6;
    double movedM = 200;          // further than this between beacons: moving
    int floodDigis = 4;           // more digipeaters than this for one packet: a flood
    double floodWindowSec = 30;
    int maxHops = 2;              // WIDEn-N above this
    int digiRegular = 10;         // relays before a digipeater counts as regular
    double digiSilentMinSec = 1800;
};

inline std::vector<std::string> splitPath(const std::string& p) {
    std::vector<std::string> v;
    std::string t;
    std::istringstream is(p);
    while (std::getline(is, t, ',')) if (!t.empty()) v.push_back(upper(t));
    return v;
}

// WIDEn-N / WIDEn: n, else -1
inline int wideHops(const std::string& tok) {
    std::string t = tok;
    if (!t.empty() && t.back() == '*') t.pop_back();
    if (t.rfind("WIDE", 0) != 0 || t.size() < 5 || !std::isdigit((unsigned char)t[4])) return -1;
    return t[4] - '0';
}

inline bool isAlias(const std::string& tok) {
    std::string t = tok;
    if (!t.empty() && t.back() == '*') t.pop_back();
    return t.rfind("WIDE", 0) == 0 || t.rfind("RELAY", 0) == 0 || t.rfind("TRACE", 0) == 0 || t.rfind("TCPIP", 0) == 0 || t.rfind("RFONLY", 0) == 0 || t == "NOGATE";
}

// what is wrong with a path, "" when nothing
inline std::string pathProblem(const std::string& path, int maxHops = 2) {
    for (const auto& tok : splitPath(path)) {
        std::string t = tok;
        if (!t.empty() && t.back() == '*') t.pop_back();
        if (t.rfind("RELAY", 0) == 0) return "RELAY (obsolete)";
        if (t.rfind("TRACE", 0) == 0) return t + " (obsolete)";
        if (t == "WIDE") return "WIDE (obsolete)";
        const int n = wideHops(t);
        if (n > maxHops) return t + " (more than " + std::to_string(maxHops) + " hops)";
    }
    return {};
}

// the call sign digipeaters in a heard path (the ones that did repeat it: up to and with the star)
inline std::vector<std::string> usedDigis(const std::string& path) {
    const auto toks = splitPath(path);
    int star = -1;
    for (int i = 0; i < (int)toks.size(); i++) if (!toks[i].empty() && toks[i].back() == '*') star = i;
    std::vector<std::string> v;
    for (int i = 0; i <= star; i++) {
        std::string t = toks[(size_t)i];
        if (!t.empty() && t.back() == '*') t.pop_back();
        if (!isAlias(t)) v.push_back(t);
    }
    return v;
}

struct AprsStationHealth {
    std::string call;
    int beacons = 0;
    double medianSec = 0;
    bool moving = false;
    bool overBeacon = false;
    std::string pathIssue;
    double lastTime = 0;
};

struct AprsDigiHealth {
    std::string call;
    int relays = 0;
    double lastTime = 0, medianGapSec = 0;
    bool silent = false;
};

class AprsHealth {
public:
    explicit AprsHealth(AprsHealthLimits l = {}) : L(l) {}

    // one decoded packet; out gets what is new (each problem once per station until it goes away)
    void add(double t, const std::string& from, const std::string& path, const std::string& info, bool hasPos, double lat, double lon,
             std::vector<Insight>& out) {
        const std::string call = upper(from);
        Station& s = st_[call];
        s.call = call; s.last = t;
        // the path
        const std::string pp = pathProblem(path, L.maxHops);
        if (!pp.empty() && s.pathIssue.empty()) {
            out.push_back(mk(t, "aprs_path", 0, call, call + " uses " + pp, hasPos, lat, lon));
        }
        s.pathIssue = pp;
        // digipeaters
        const auto digis = usedDigis(path);
        for (const auto& d : digis) {
            Digi& g = dg_[d];
            if (g.last > 0 && t > g.last) { g.gaps.push_back(t - g.last); if (g.gaps.size() > 60) g.gaps.pop_front(); }
            g.last = t; g.relays++; g.silentSaid = false;
        }
        // the same packet through several digipeaters
        const std::string pk = call + '\x1f' + info;
        auto& fl = flood_[pk];
        if (fl.first == 0 || t - fl.first > L.floodWindowSec) { fl = Flood(); fl.first = t; }
        // the digipeater that sent this copy: the starred call, else the path text itself
        fl.via.insert(digis.empty() ? path : digis.back());
        if ((int)fl.via.size() > L.floodDigis && !fl.said) {
            fl.said = true;
            out.push_back(mk(t, "aprs_flood", 1, call, call + ": one packet through " + std::to_string(fl.via.size()) + " digipeaters in " +
                                                       std::to_string((int)(t - fl.first)) + " s", hasPos, lat, lon));
        }
        if (flood_.size() > 4000) trimFloods(t);
        // beacons: position packets only
        if (hasPos) {
            if (s.havePos) {
                const double m = distanceKm(s.lat, s.lon, lat, lon) * 1000;
                s.moved.push_back(m > L.movedM);
                if (s.moved.size() > 12) s.moved.pop_front();
                if (t > s.lastPosT) { s.gaps.push_back(t - s.lastPosT); if (s.gaps.size() > 12) s.gaps.pop_front(); }
            }
            s.havePos = true; s.lat = lat; s.lon = lon; s.lastPosT = t; s.beacons++;
            const bool was = s.over;
            s.over = overBeacon(s);
            if (s.over && !was) {
                const double med = median(s.gaps);
                out.push_back(mk(t, "aprs_overbeacon", 1, call, call + " beacons every " + secText(med) + (moving(s) ? " while moving" : " from a fixed place") +
                                                               " (limit " + secText(moving(s) ? L.movingMinSec : L.fixedMinSec) + ")", true, lat, lon));
            }
        }
    }

    // digipeaters gone quiet; call now and then
    void tick(double now, std::vector<Insight>& out) {
        for (auto& kv : dg_) {
            Digi& g = kv.second;
            if (g.relays < L.digiRegular || g.gaps.empty() || g.silentSaid) continue;
            const double med = median(g.gaps), lim = std::max(L.digiSilentMinSec, 6 * med);
            if (now - g.last > lim) {
                g.silentSaid = true;
                out.push_back(mk(now, "aprs_digi_silent", 1, kv.first, "Digipeater " + kv.first + " silent for " + secText(now - g.last) + " (it relayed every " + secText(med) + ")", false, 0, 0));
            }
        }
    }

    std::vector<AprsStationHealth> stations() const {
        std::vector<AprsStationHealth> v;
        for (const auto& kv : st_) {
            const Station& s = kv.second;
            AprsStationHealth h;
            h.call = s.call; h.beacons = s.beacons; h.medianSec = median(s.gaps); h.moving = moving(s); h.overBeacon = s.over; h.pathIssue = s.pathIssue; h.lastTime = s.last;
            v.push_back(h);
        }
        return v;
    }
    std::vector<AprsDigiHealth> digis(double now) const {
        std::vector<AprsDigiHealth> v;
        for (const auto& kv : dg_) {
            const Digi& g = kv.second;
            AprsDigiHealth d;
            d.call = kv.first; d.relays = g.relays; d.lastTime = g.last; d.medianGapSec = median(g.gaps);
            d.silent = g.relays >= L.digiRegular && !g.gaps.empty() && now - g.last > std::max(L.digiSilentMinSec, 6 * d.medianGapSec);
            v.push_back(d);
        }
        return v;
    }
    void clear() { st_.clear(); dg_.clear(); flood_.clear(); }

    static std::string secText(double s) {
        char b[32];
        if (s < 120) snprintf(b, sizeof b, "%.0f s", s);
        else if (s < 7200) snprintf(b, sizeof b, "%.0f min", s / 60);
        else snprintf(b, sizeof b, "%.1f h", s / 3600);
        return b;
    }

private:
    struct Station {
        std::string call, pathIssue;
        double last = 0, lastPosT = 0, lat = 0, lon = 0;
        bool havePos = false, over = false;
        int beacons = 0;
        std::deque<double> gaps;
        std::deque<bool> moved;
    };
    struct Digi { double last = 0; int relays = 0; std::deque<double> gaps; bool silentSaid = false; };
    struct Flood { double first = 0; std::set<std::string> via; bool said = false; };

    template <class C> static double median(const C& c) {
        if (c.empty()) return 0;
        std::vector<double> v(c.begin(), c.end());
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    }
    static bool moving(const Station& s) {
        int m = 0;
        for (bool b : s.moved) m += b;
        return !s.moved.empty() && m * 2 > (int)s.moved.size();
    }
    bool overBeacon(const Station& s) const {
        if (s.beacons < L.beaconsNeeded || (int)s.gaps.size() < L.beaconsNeeded - 1) return false;
        // the gaps of a fixed station that only repeats the same position are not shortened by digipeated copies: under 5 s apart is one beacon heard twice
        std::vector<double> g;
        for (double x : s.gaps) if (x >= 5) g.push_back(x);
        if ((int)g.size() < L.beaconsNeeded - 1) return false;
        return median(g) < (moving(s) ? L.movingMinSec : L.fixedMinSec) * 0.95;
    }
    void trimFloods(double t) {
        for (auto it = flood_.begin(); it != flood_.end();) it = t - it->second.first > L.floodWindowSec ? flood_.erase(it) : std::next(it);
    }
    static Insight mk(double t, const char* kind, int sev, const std::string& key, const std::string& text, bool hasPos, double lat, double lon) {
        Insight i;
        i.time = t; i.area = "ham"; i.kind = kind; i.severity = sev; i.key = key; i.text = text; i.hasPos = hasPos; i.lat = lat; i.lon = lon;
        return i;
    }

    AprsHealthLimits L;
    std::map<std::string, Station> st_;
    std::map<std::string, Digi> dg_;
    std::map<std::string, Flood> flood_;
};

}  // namespace dect2::insight
