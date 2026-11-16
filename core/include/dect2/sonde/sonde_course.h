// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The predicted rest of a radiosonde's flight as a list of positions with a flight time each, and that list as a stream of NMEA GGA / RMC
// sentences for a map program that takes several GPS inputs ("Predicted course" output). Pure functions, no sockets (sonde_nmea_out.h
// sends them). The prediction gives a ground track and a landing time only, so the height is taken as a straight line from the sonde's
// height now to the ground at the landing, spread along the path: a guide for drawing, not a measured profile.
#pragma once
#include "dect2/sonde/sonde_nmea.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {
namespace sondecourse {

struct Point { double lat = 0, lon = 0, altM = 0, t = 0; };   // t: seconds of flight from now

inline double distM(double la1, double lo1, double la2, double lo2) {   // great circle, spherical earth
    const double r = 3.14159265358979323846 / 180.0, a = (la2 - la1) * r / 2, b = (lo2 - lo1) * r / 2;
    const double h = std::sin(a) * std::sin(a) + std::cos(la1 * r) * std::cos(la2 * r) * std::sin(b) * std::sin(b);
    return 2 * 6371000.0 * std::asin(std::min(1.0, std::sqrt(h)));
}
inline double bearingDeg(double la1, double lo1, double la2, double lo2) {
    const double r = 3.14159265358979323846 / 180.0;
    const double y = std::sin((lo2 - lo1) * r) * std::cos(la2 * r), x = std::cos(la1 * r) * std::sin(la2 * r) - std::sin(la1 * r) * std::cos(la2 * r) * std::cos((lo2 - lo1) * r);
    double d = std::atan2(y, x) / r;
    if (d < 0) d += 360;
    return d;
}

// The course from the sonde's position to the landing: start, the path points between (nearly equal neighbours dropped), landing.
// Height and flight time are spread along the path by distance. Always at least two points.
inline std::vector<Point> build(double lat0, double lon0, double alt0, double groundM, double landLat, double landLon, double secondsLeft, const std::vector<std::pair<double, double>>& path) {
    std::vector<std::pair<double, double>> ll;
    ll.push_back({lat0, lon0});
    for (const auto& p : path) {
        if (!std::isfinite(p.first) || !std::isfinite(p.second)) continue;
        if (distM(ll.back().first, ll.back().second, p.first, p.second) < 25 || distM(p.first, p.second, landLat, landLon) < 25) continue;
        ll.push_back(p);
    }
    ll.push_back({landLat, landLon});
    std::vector<double> cum(ll.size(), 0.0);
    for (size_t i = 1; i < ll.size(); i++) cum[i] = cum[i - 1] + distM(ll[i - 1].first, ll[i - 1].second, ll[i].first, ll[i].second);
    const double total = cum.back();
    std::vector<Point> out;
    for (size_t i = 0; i < ll.size(); i++) {
        const double f = total > 0 ? cum[i] / total : (i + 1 == ll.size() ? 1.0 : 0.0);
        Point q; q.lat = ll[i].first; q.lon = ll[i].second; q.altM = alt0 + (groundM - alt0) * f; q.t = std::max(0.0, secondsLeft) * f;
        out.push_back(q);
    }
    return out;
}

// Points at flight time 0, stepS, 2 stepS ... and then the last point (the landing), linear between the course points.
inline std::vector<Point> resample(const std::vector<Point>& c, double stepS) {
    std::vector<Point> out;
    if (c.empty()) return out;
    if (!(stepS >= 1)) stepS = 60;
    const double end = c.back().t;
    size_t j = 0;
    for (double t = 0; t < end && out.size() < 5000; t += stepS) {
        while (j + 1 < c.size() && c[j + 1].t <= t) j++;
        if (j + 1 >= c.size()) break;
        const Point &a = c[j], &b = c[j + 1];
        const double f = b.t > a.t ? (t - a.t) / (b.t - a.t) : 0.0;
        Point q; q.lat = a.lat + (b.lat - a.lat) * f; q.lon = a.lon + (b.lon - a.lon) * f; q.altM = a.altM + (b.altM - a.altM) * f; q.t = t;
        out.push_back(q);
    }
    out.push_back(c.back());
    return out;
}

// One GGA + RMC pair per point. The sentence time is startUnix + flight time; the RMC speed and course are those towards the next point
// (the last point keeps the course before it, speed 0).
inline std::vector<std::string> sentencesFor(const std::vector<Point>& pts, double startUnix) {
    std::vector<std::string> out;
    for (size_t i = 0; i < pts.size(); i++) {
        SondeInfo s;
        s.hasPos = true; s.lat = pts[i].lat; s.lon = pts[i].lon; s.altM = pts[i].altM; s.hasTime = true; s.unixTime = startUnix + pts[i].t; s.sats = -1; s.lastHeardS = 0;
        const size_t a = i + 1 < pts.size() ? i : (i > 0 ? i - 1 : i), b = i + 1 < pts.size() ? i + 1 : i;
        if (a != b) {
            const double dt = std::max(1e-3, pts[b].t - pts[a].t);
            s.hasVel = true; s.headingDeg = bearingDeg(pts[a].lat, pts[a].lon, pts[b].lat, pts[b].lon);
            s.hSpeed = i + 1 < pts.size() ? distM(pts[a].lat, pts[a].lon, pts[b].lat, pts[b].lon) / dt : 0;
        }
        const std::string t = sondenmea::sentences(s, startUnix, 1e9);
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

// "Aim point": the landing alone.
inline std::string aimSentences(const Point& landing, double startUnix) { auto v = sentencesFor({landing}, startUnix + 0); return v.empty() ? std::string() : v[0]; }

// Hands out the sentences one after the other and starts over after the last. set() keeps the place when the list is replaced.
class Sequencer {
public:
    void set(std::vector<std::string> v) { seq_ = std::move(v); if (!seq_.empty()) i_ %= seq_.size(); else i_ = 0; }
    void restart() { i_ = 0; }
    size_t size() const { return seq_.size(); }
    size_t index() const { return i_; }
    std::string next() { if (seq_.empty()) return {}; const std::string& s = seq_[i_]; i_ = (i_ + 1) % seq_.size(); return s; }
private:
    std::vector<std::string> seq_;
    size_t i_ = 0;
};

}  // namespace sondecourse
}  // namespace dect2
