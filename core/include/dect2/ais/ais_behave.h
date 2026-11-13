// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AIS behaviour flags and anchor watch (3.9, docs/SMART5-marine-3.9.md). These are about what a vessel is doing, next to the anomaly engine
// (ais_anomaly.h), which is about whether its reports can be true:
//  - "rendezvous":   two vessels within rendezvousNm of each other, both under 1 kn, for rendezvousMin minutes (a ship-to-ship transfer,
//                    a tow being connected, fishing boats rafted up); neither may be moored or at anchor and not within 2 nm of the
//                    position where either was first heard stationary for long (a port berth);
//  - "loiter":       under way (status 0 / 8 / 15 or none) and staying inside loiterNm for loiterMin minutes while moving (a search pattern,
//                    a survey, waiting off a port without anchoring);
//  - "status_moving": status moored (5) or at anchor (1) but making more than 3 kn, three reports in a row (status not updated, or dragging);
//  - "status_stopped": status "under way using engine" (0) but under 0.3 kn for stoppedMin minutes;
//  - "draught":      the declared draught changed by 0.5 m or more (loaded or unloaded);
//  - "destination":  the declared destination changed to another word;
//  - "anchor_drag":  at anchor (status 1, or stopped under 0.5 kn for 20 min): its swing circle is learned from the first 20 minutes; leaving
//                    it by more than dragM, on three reports in a row and moving steadily one way, is "dragging anchor".
// Pure logic, no clock.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct AisBehaveReport {         // one position report as the receiver has it
    uint32_t mmsi = 0;
    double t = 0;                // seconds
    double lat = 0, lon = 0;
    double sog = -1;             // kn, -1 unknown
    int navStatus = -1;
    double draughtM = 0;         // 0 unknown
    std::string destination, name;
    bool isShip = true;          // false for aids, base stations, SART, aircraft
};

struct AisBehaveFlag {
    std::string kind;
    uint32_t mmsi = 0, other = 0;
    double t = 0, lat = 0, lon = 0;
    double value = 0;            // m from the anchor, metres change of draught, minutes ...
    std::string text;
};

class AisBehaviour {
public:
    double rendezvousNm = 0.3, rendezvousMin = 30;
    double loiterNm = 1.0, loiterMin = 120;
    double stoppedMin = 60;
    double dragM = 80;           // beyond the learned swing radius
    double cooldownSec = 3 * 3600;

    void add(const AisBehaveReport& r, std::vector<AisBehaveFlag>& out) {
        if (!r.isShip || r.mmsi == 0) return;
        V& v = v_[r.mmsi];
        if (v.lastT > -1e8 && r.t - v.lastT > 6 * 3600) v = V();   // a vessel back after a long time starts over
        v.lastT = r.t; v.lat = r.lat; v.lon = r.lon; v.sog = r.sog; v.status = r.navStatus;
        if (!r.name.empty()) v.name = r.name;
        v.hist.push_back({r.t, r.lat, r.lon, r.sog});
        while (!v.hist.empty() && r.t - v.hist.front().t > std::max(loiterMin, rendezvousMin) * 60 + 600) v.hist.pop_front();
        const std::string who = label(r.mmsi, v.name);
        char b[240];
        // draught and destination
        if (r.draughtM > 0) {
            if (v.draught > 0 && std::fabs(r.draughtM - v.draught) >= 0.5) {
                snprintf(b, sizeof b, "%s draught %.1f -> %.1f m (%s)", who.c_str(), v.draught, r.draughtM, r.draughtM > v.draught ? "loaded" : "unloaded");
                flag(out, "draught", r, 0, r.draughtM - v.draught, b, 0);
            }
            v.draught = r.draughtM;
        }
        const std::string d = norm(r.destination);
        if (!d.empty()) {
            if (!v.dest.empty() && d != v.dest) { snprintf(b, sizeof b, "%s destination %s -> %s", who.c_str(), v.dest.c_str(), d.c_str()); flag(out, "destination", r, 0, 0, b, 0); }
            v.dest = d;
        }
        // status against motion
        if ((r.navStatus == 1 || r.navStatus == 5) && r.sog > 3) {
            if (++v.movingAtStatus >= 3) { snprintf(b, sizeof b, "%s reports '%s' but makes %.1f kn", who.c_str(), r.navStatus == 1 ? "at anchor" : "moored", r.sog); flag(out, "status_moving", r, 0, r.sog, b); }
        } else v.movingAtStatus = 0;
        if (r.navStatus == 0 && r.sog >= 0 && r.sog < 0.3) {
            if (v.stoppedSince < 0) v.stoppedSince = r.t;
            if (r.t - v.stoppedSince >= stoppedMin * 60) { snprintf(b, sizeof b, "%s reports 'under way using engine' but has been stopped for %.0f min", who.c_str(), (r.t - v.stoppedSince) / 60); flag(out, "status_stopped", r, 0, (r.t - v.stoppedSince) / 60, b); }
        } else v.stoppedSince = -1;
        // loitering: moving (over 1 kn on average) but every position of the last loiterMin inside loiterNm of their centre
        const bool underWay = r.navStatus < 0 || r.navStatus == 0 || r.navStatus == 8 || r.navStatus == 15 || r.navStatus == 7;
        if (underWay && !v.hist.empty() && r.t - v.hist.front().t >= loiterMin * 60) {
            double cl = 0, co = 0, ss = 0; int n = 0;
            for (const auto& p : v.hist) if (r.t - p.t <= loiterMin * 60) { cl += p.lat; co += p.lon; ss += std::max(0.0, p.sog); n++; }
            if (n >= 10 && ss / n > 1.5) {
                cl /= n; co /= n; double mx = 0;
                for (const auto& p : v.hist) if (r.t - p.t <= loiterMin * 60) mx = std::max(mx, nm(p.lat, p.lon, cl, co));
                if (mx < loiterNm) { snprintf(b, sizeof b, "%s has stayed within %.1f nm for %.0f min while making %.1f kn", who.c_str(), mx, loiterMin, ss / n); flag(out, "loiter", r, 0, loiterMin, b); }
            }
        }
        // stationary-for-long places: the berths and anchorages of this receiver's area (a rendezvous there is ordinary)
        if (r.sog >= 0 && r.sog < 0.5 && (r.navStatus == 5 || r.navStatus == 1)) noteBerth(r.lat, r.lon);
        // rendezvous with any other vessel
        if (r.sog >= 0 && r.sog < 1 && r.navStatus != 1 && r.navStatus != 5 && !nearBerth(r.lat, r.lon)) {
            for (auto& [m2, o] : v_) {
                if (m2 == r.mmsi || r.t - o.lastT > 600 || o.sog < 0 || o.sog >= 1 || o.status == 1 || o.status == 5) continue;
                const uint64_t key = r.mmsi < m2 ? ((uint64_t)r.mmsi << 32 | m2) : ((uint64_t)m2 << 32 | r.mmsi);
                double& since = pairs_.try_emplace(key, -1.0).first->second;
                if (nm(r.lat, r.lon, o.lat, o.lon) > rendezvousNm) { since = -1; continue; }
                if (since < 0) since = r.t;
                if (r.t - since >= rendezvousMin * 60) {
                    snprintf(b, sizeof b, "%s and %s side by side, stopped, for %.0f min", who.c_str(), label(m2, o.name).c_str(), (r.t - since) / 60);
                    flag(out, "rendezvous", r, m2, (r.t - since) / 60, b);
                }
            }
        }
        // anchor watch
        const bool anchored = r.navStatus == 1 || (r.sog >= 0 && r.sog < 0.5 && r.navStatus != 5 && v.stillSince >= 0 && r.t - v.stillSince >= 1200);
        if (r.sog >= 0 && r.sog < 0.5) { if (v.stillSince < 0) v.stillSince = r.t; } else if (r.navStatus != 1) { v.stillSince = -1; v.anc = Anchor(); }
        if (anchored) {
            Anchor& a = v.anc;
            if (a.start < 0) a.start = r.t;
            if (r.t - a.start < 1200) { a.pts.push_back({r.lat, r.lon}); }
            else {
                if (!a.learned) {
                    if (a.pts.size() >= 5) {
                        for (auto& p : a.pts) { a.clat += p.first; a.clon += p.second; }
                        a.clat /= a.pts.size(); a.clon /= a.pts.size();
                        for (auto& p : a.pts) a.radius = std::max(a.radius, nm(p.first, p.second, a.clat, a.clon) * 1852);
                        a.learned = true;
                    } else { a = Anchor(); a.start = r.t; }
                }
                if (a.learned) {
                    const double dm = nm(r.lat, r.lon, a.clat, a.clon) * 1852;
                    if (dm > a.radius + dragM && dm > a.lastOut) {
                        if (++a.outCount >= 3) {
                            const double brg = std::fmod(std::atan2((r.lon - a.clon) * std::cos(r.lat * M_PI / 180), r.lat - a.clat) * 180 / M_PI + 360, 360);
                            snprintf(b, sizeof b, "%s dragging anchor: %.0f m from its anchor position (swing radius %.0f m), moving towards %03.0f", who.c_str(), dm, a.radius, brg);
                            flag(out, "anchor_drag", r, 0, dm, b);
                        }
                    } else if (dm <= a.radius + dragM) a.outCount = 0;
                    a.lastOut = dm;
                }
            }
        }
    }
    void clear() { v_.clear(); pairs_.clear(); last_.clear(); berths_.clear(); }
    static double nm(double la1, double lo1, double la2, double lo2) {
        const double r = M_PI / 180, x = (lo2 - lo1) * r * std::cos((la1 + la2) * 0.5 * r), y = (la2 - la1) * r;
        return 3440.065 * std::hypot(x, y);
    }

private:
    struct P { double t, lat, lon, sog; };
    struct Anchor { double start = -1; std::vector<std::pair<double, double>> pts; bool learned = false; double clat = 0, clon = 0, radius = 0, lastOut = 0; int outCount = 0; };
    struct V { double lastT = -1e9, lat = 0, lon = 0, sog = -1; int status = -1; std::string name, dest; double draught = 0; int movingAtStatus = 0;
               double stoppedSince = -1, stillSince = -1; std::deque<P> hist; Anchor anc; };
    std::map<uint32_t, V> v_;
    std::map<uint64_t, double> pairs_;
    std::map<std::string, double> last_;
    std::vector<std::pair<double, double>> berths_;
    static std::string norm(const std::string& s) { std::string o; for (char c : s) if (std::isalnum((unsigned char)c)) o += (char)std::toupper((unsigned char)c); return o; }
    static std::string label(uint32_t m, const std::string& n) { char b[16]; snprintf(b, sizeof b, "%09u", m); return n.empty() ? b : n + " (" + b + ")"; }
    void noteBerth(double lat, double lon) { if (!nearBerth(lat, lon) && berths_.size() < 2000) berths_.push_back({lat, lon}); }
    bool nearBerth(double lat, double lon) const { for (auto& b : berths_) if (nm(lat, lon, b.first, b.second) < 2) return true; return false; }
    void flag(std::vector<AisBehaveFlag>& out, const char* kind, const AisBehaveReport& r, uint32_t other, double value, const char* text, double cooldown = -1) {
        const std::string key = std::string(kind) + "|" + std::to_string(std::min(r.mmsi, other ? other : r.mmsi)) + "|" + std::to_string(std::max(r.mmsi, other));
        const double cd = cooldown < 0 ? cooldownSec : cooldown;
        auto it = last_.find(key);
        if (it != last_.end() && r.t - it->second < cd) return;
        last_[key] = r.t;
        AisBehaveFlag f; f.kind = kind; f.mmsi = r.mmsi; f.other = other; f.t = r.t; f.lat = r.lat; f.lon = r.lon; f.value = value; f.text = text;
        out.push_back(f);
    }
};

} // namespace dect2
