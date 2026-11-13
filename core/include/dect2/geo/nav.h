// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Car navigation from a position fix: routes (waypoints clicked on the map, or a GPX file), progress along a route (distance and bearing to the
// next waypoint, distance left, ETA, off-route), dead-reckoning smoothing between fixes, and a GPX trip log. Pure maths and text, header only,
// no radio and no UI. Accuracy is whatever the fix gives; nothing here is a safety function.
#pragma once
#include "kinematics.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace dect2::geo {

struct LatLon { double lat = 0, lon = 0; };

struct Route {
    std::string name;
    std::vector<LatLon> pts;      // waypoints in order; the course is the straight line from one to the next
};

inline double navDistM(LatLon a, LatLon b) { return geodesicInverse(a.lat, a.lon, b.lat, b.lon).distM; }
inline double navBearing(LatLon a, LatLon b) { return geodesicInverse(a.lat, a.lon, b.lat, b.lon).az1Deg; }
inline double routeLengthM(const Route& r) { double s = 0; for (size_t i = 1; i < r.pts.size(); i++) s += navDistM(r.pts[i - 1], r.pts[i]); return s; }

// the point `distM` metres from `p` along `bearingDeg` (a sphere: good to metres over the short hops of dead reckoning)
inline LatLon navAhead(LatLon p, double bearingDeg, double distM) {
    const double r = distM / 6371008.8, h = bearingDeg * kKinDeg, p1 = p.lat * kKinDeg;
    const double la = std::asin(std::sin(p1) * std::cos(r) + std::cos(p1) * std::sin(r) * std::cos(h));
    const double lo = p.lon * kKinDeg + std::atan2(std::sin(h) * std::sin(r) * std::cos(p1), std::cos(r) - std::sin(p1) * std::sin(la));
    return {la / kKinDeg, kinWrap180(lo / kKinDeg)};
}

// ---- progress along a route ----
struct NavProgress {
    bool valid = false;
    int seg = 0;                 // the leg being driven: pts[seg] to pts[seg + 1]
    int next = 1;                // the next waypoint (pts[next])
    double crossM = 0;           // distance from the route line (always >= 0)
    double alongM = 0;           // distance driven along the route
    double leftM = 0;            // distance left to the end by the route
    double toNextM = 0;          // straight distance to the next waypoint
    double bearingToNext = 0;    // degrees true
    double turnDeg = 0;          // course change needed to head for the next waypoint, -180..180 (right positive); 0 when the course is not known
    double etaS = -1;            // seconds left at the given speed (-1 when standing still)
    bool arrived = false;        // within the arrival radius of the last waypoint
    bool offRoute = false;       // further from the line than the limit
};

struct NavSettings {
    double offRouteM = 50;       // beyond this from the line (and beyond 3 sigma of the fix) the course is "off route"
    double reachedM = 25;        // a waypoint counts as reached within this
    double minEtaSpeedMps = 0.5; // below this no ETA
};

// local flat metres east/north of `o` (good to a fraction of a percent over a few kilometres)
inline void navLocalXY(LatLon o, LatLon p, double& x, double& y) {
    const double c = std::cos(o.lat * kKinDeg);
    x = kinWrap180(p.lon - o.lon) * kKinDeg * 6371008.8 * c;
    y = (p.lat - o.lat) * kKinDeg * 6371008.8;
}

// Where `pos` stands on `r`. `hintSeg` (the leg last driven) restricts the search to a window around it so that a route which crosses
// itself or comes back does not jump ahead; pass -1 for a global search. `sigmaM` is the 1-sigma horizontal error of the fix.
inline NavProgress navProgress(const Route& r, LatLon pos, double speedMps, double courseDeg, bool courseKnown, double sigmaM, int hintSeg, const NavSettings& s = {}) {
    NavProgress n;
    if (r.pts.size() < 2) return n;
    const int segs = (int)r.pts.size() - 1;
    int lo = 0, hi = segs - 1;
    if (hintSeg >= 0) { lo = std::max(0, hintSeg - 1); hi = std::min(segs - 1, hintSeg + 3); }
    double best = 1e300, bestT = 0;
    int bestSeg = lo;
    for (int i = lo; i <= hi; i++) {
        double bx, by, px, py;
        navLocalXY(pos, r.pts[(size_t)i], px, py);
        navLocalXY(pos, r.pts[(size_t)i + 1], bx, by);
        const double dx = bx - px, dy = by - py, L2 = dx * dx + dy * dy;
        double t = L2 > 0 ? (-px * dx - py * dy) / L2 : 0;
        t = std::max(0.0, std::min(1.0, t));
        const double d = std::hypot(px + t * dx, py + t * dy);
        if (d < best) { best = d; bestSeg = i; bestT = t; }
    }
    n.valid = true;
    n.seg = bestSeg;
    n.crossM = best;
    const double legLen = navDistM(r.pts[(size_t)bestSeg], r.pts[(size_t)bestSeg + 1]);
    double before = 0;
    for (int i = 0; i < bestSeg; i++) before += navDistM(r.pts[(size_t)i], r.pts[(size_t)i + 1]);
    n.alongM = before + bestT * legLen;
    n.leftM = std::max(0.0, routeLengthM(r) - n.alongM);
    n.next = bestSeg + 1;
    n.toNextM = navDistM(pos, r.pts[(size_t)n.next]);
    // a waypoint close by is reached: head for the one after it
    if (n.toNextM < s.reachedM && n.next < (int)r.pts.size() - 1) { n.next++; n.seg = n.next - 1; n.toNextM = navDistM(pos, r.pts[(size_t)n.next]); }
    n.bearingToNext = navBearing(pos, r.pts[(size_t)n.next]);
    if (courseKnown) n.turnDeg = kinWrap180(n.bearingToNext - courseDeg);
    n.arrived = n.next == (int)r.pts.size() - 1 && n.toNextM < s.reachedM;
    n.offRoute = !n.arrived && best > std::max(s.offRouteM, 3 * sigmaM);
    n.etaS = speedMps >= s.minEtaSpeedMps ? n.leftM / speedMps : -1;
    return n;
}

// ---- dead reckoning ----
// Between fixes (about one a second) the position is carried on along the last course at the last speed, up to `coastMaxS`; a new fix is blended
// in so the marker does not jump. No fix (or none for longer than the coast) is reported as such, never as a position.
struct DeadReckoner {
    enum State { NoFix, Fix, Coasting };
    double coastMaxS = 10;       // after this long without a fix the position is dropped
    double blend = 0.6;          // share of a new fix taken at once (1 = no smoothing)
    State state = NoFix;
    LatLon pos;                  // the smoothed position, valid unless NoFix
    double speedMps = 0, courseDeg = 0;
    bool courseKnown = false;
    double sigmaM = 0;           // 1-sigma of the last fix; grows while coasting
    double fixT = -1e9, lastT = -1e9;

    // call whenever the receiver reports; `t` is any steady clock in seconds. A fix with hasVelocity false keeps the speed worked out from the move.
    void onFix(double t, LatLon f, double hSigmaM, bool hasVelocity, double speed, double course) {
        if (state == NoFix) { pos = f; }
        else {
            // carry the old position to now first, then pull it towards the fix
            advance(t);
            pos.lat += (f.lat - pos.lat) * blend;
            pos.lon += (f.lon - pos.lon) * blend;
        }
        if (hasVelocity) { speedMps = speed; courseDeg = course; courseKnown = speed > 0.8; }
        else if (state != NoFix) {
            const Geodesic g = geodesicInverse(prevFix.lat, prevFix.lon, f.lat, f.lon);
            const double dt = t - fixT;
            if (dt > 0.05 && dt < 10) { speedMps = g.distM / dt; if (g.distM > 3 * hSigmaM * 0.2 + 1) { courseDeg = g.az1Deg; courseKnown = true; } else courseKnown = false; }
        }
        prevFix = f;
        sigmaM = hSigmaM;
        fixT = lastT = t;
        state = Fix;
    }
    void advance(double t) {
        const double dt = t - lastT;
        if (dt > 0 && courseKnown && speedMps > 0.5 && state != NoFix) pos = navAhead(pos, courseDeg, speedMps * dt);
        if (dt > 0) lastT = t;
    }
    // call every frame
    void tick(double t) {
        if (state == NoFix) return;
        if (t - fixT > coastMaxS) { state = NoFix; return; }
        if (t - fixT > 1.5) { advance(t); state = Coasting; }
        else { advance(t); state = Fix; }
    }
    void clear() { state = NoFix; fixT = lastT = -1e9; }

private:
    LatLon prevFix;
};

// ---- GPX ----
inline std::string gpxEscape(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '"') o += "&quot;"; else o += c; }
    return o;
}

inline double gpxAttr(const std::string& tag, const char* name, bool& ok) {
    size_t p = 0;
    const std::string key = std::string(name) + "=";
    while ((p = tag.find(key, p)) != std::string::npos) {
        if (p > 0 && tag[p - 1] != ' ' && tag[p - 1] != '\t' && tag[p - 1] != '\n' && tag[p - 1] != '\r') { p += key.size(); continue; }
        const size_t q = p + key.size();
        if (q >= tag.size() || (tag[q] != '"' && tag[q] != '\'')) { p = q; continue; }
        const size_t e = tag.find(tag[q], q + 1);
        if (e == std::string::npos) break;
        char* end = nullptr;
        const std::string v = tag.substr(q + 1, e - q - 1);
        const double d = std::strtod(v.c_str(), &end);
        ok = end && end != v.c_str();
        return d;
    }
    ok = false;
    return 0;
}

// Read a GPX document: the first route (<rte>) if there is one, else the first track (<trk>, all its segments), else the waypoints (<wpt>).
// A track longer than `maxPts` is thinned by keeping every n-th point (and the last) so the route stays light. Returns false when no point is found.
inline bool parseGpx(const std::string& xml, Route& out, size_t maxPts = 4000) {
    out = Route();
    auto collect = [&](const char* open, const char* close, const char* pt, std::vector<LatLon>& v) {
        size_t a = xml.find(open);
        if (a == std::string::npos) return false;
        size_t b = xml.find(close, a);
        if (b == std::string::npos) b = xml.size();
        const std::string ptOpen = std::string("<") + pt;
        size_t p = a;
        while ((p = xml.find(ptOpen, p)) != std::string::npos && p < b) {
            const size_t e = xml.find('>', p);
            if (e == std::string::npos) break;
            const char nx = xml[p + ptOpen.size()];
            if (nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' || nx == '>' || nx == '/') {
                const std::string tag = xml.substr(p, e - p);
                bool o1, o2;
                const double la = gpxAttr(tag, "lat", o1), lo = gpxAttr(tag, "lon", o2);
                if (o1 && o2 && std::fabs(la) <= 90 && std::fabs(lo) <= 180) v.push_back({la, lo});
            }
            p = e;
        }
        return !v.empty();
    };
    std::vector<LatLon> v;
    if (!collect("<rte", "</rte>", "rtept", v)) {
        // a track has several segments: take every trkpt inside the first <trk>
        if (!collect("<trk", "</trk>", "trkpt", v)) {
            // waypoints anywhere in the file
            size_t p = 0;
            while ((p = xml.find("<wpt", p)) != std::string::npos) {
                const size_t e = xml.find('>', p);
                if (e == std::string::npos) break;
                const char nx = xml[p + 4];
                if (nx == ' ' || nx == '\t' || nx == '\n' || nx == '\r' || nx == '>') {
                    const std::string tag = xml.substr(p, e - p);
                    bool o1, o2;
                    const double la = gpxAttr(tag, "lat", o1), lo = gpxAttr(tag, "lon", o2);
                    if (o1 && o2 && std::fabs(la) <= 90 && std::fabs(lo) <= 180) v.push_back({la, lo});
                }
                p = e;
            }
        }
    }
    if (v.empty()) return false;
    if (v.size() > maxPts) {
        const size_t step = (v.size() + maxPts - 1) / maxPts;
        std::vector<LatLon> t;
        for (size_t i = 0; i < v.size(); i += step) t.push_back(v[i]);
        if (t.back().lat != v.back().lat || t.back().lon != v.back().lon) t.push_back(v.back());
        v.swap(t);
    }
    // the name: the first <name> after the route/track opening (none is fine)
    const size_t nb = xml.find("<name>");
    if (nb != std::string::npos) { const size_t ne = xml.find("</name>", nb); if (ne != std::string::npos) out.name = xml.substr(nb + 6, ne - nb - 6); }
    out.pts = std::move(v);
    return true;
}

inline std::string gpxRoute(const Route& r) {
    std::string o = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<gpx version=\"1.1\" creator=\"OnAir\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n<rte><name>" + gpxEscape(r.name) + "</name>\n";
    char b[96];
    for (const LatLon& p : r.pts) { snprintf(b, sizeof b, "<rtept lat=\"%.7f\" lon=\"%.7f\"/>\n", p.lat, p.lon); o += b; }
    return o + "</rte>\n</gpx>\n";
}

// ---- trip log ----
struct TripPoint { double lat, lon, ele; double utc; double speedMps; };   // utc: unix seconds, <= 0 when the clock is not known

struct TripLog {
    std::vector<TripPoint> pts;
    double minMoveM = 5, minGapS = 5;     // a point is added after this far or this long, whichever comes first
    double distM = 0;
    bool add(double lat, double lon, double ele, double utc, double speed) {
        if (!pts.empty()) {
            const TripPoint& l = pts.back();
            const double d = navDistM({l.lat, l.lon}, {lat, lon});
            const bool gap = utc > 0 && l.utc > 0 && utc - l.utc >= minGapS;
            if (d < minMoveM && !gap) return false;
            if (d < 1e5) distM += d;
        }
        pts.push_back({lat, lon, ele, utc, speed});
        if (pts.size() > 200000) pts.erase(pts.begin(), pts.begin() + 1000);
        return true;
    }
    void clear() { pts.clear(); distM = 0; }
};

inline std::string gpxTime(double utc) {
    time_t t = (time_t)utc;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char b[40];
    snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02dZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

inline std::string gpxTrack(const TripLog& log, const std::string& name) {
    std::string o = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<gpx version=\"1.1\" creator=\"OnAir\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n<trk><name>" + gpxEscape(name) + "</name><trkseg>\n";
    char b[160];
    for (const TripPoint& p : log.pts) {
        snprintf(b, sizeof b, "<trkpt lat=\"%.7f\" lon=\"%.7f\"><ele>%.1f</ele>", p.lat, p.lon, p.ele);
        o += b;
        if (p.utc > 0) o += "<time>" + gpxTime(p.utc) + "</time>";
        o += "</trkpt>\n";
    }
    return o + "</trkseg></trk>\n</gpx>\n";
}

// ---- text for the screen and the prompts ----
inline std::string navDistText(double m) {
    char b[32];
    if (m < 1000) snprintf(b, sizeof b, "%.0f m", m < 100 ? std::round(m / 5) * 5 : std::round(m / 10) * 10);
    else if (m < 10000) snprintf(b, sizeof b, "%.1f km", m / 1000);
    else snprintf(b, sizeof b, "%.0f km", m / 1000);
    return b;
}
inline std::string navEtaText(double s) {
    if (s < 0) return "-";
    char b[32];
    const int m = (int)std::lround(s / 60);
    if (m < 60) snprintf(b, sizeof b, "%d min", std::max(m, 0)); else snprintf(b, sizeof b, "%d h %02d min", m / 60, m % 60);
    return b;
}
inline const char* navCompass(double deg) {
    static const char* const n[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return n[(int)std::lround(kinWrap360(deg) / 45.0) % 8];
}

} // namespace dect2::geo
