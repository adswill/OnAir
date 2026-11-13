// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Smart insights (docs/SMART5-*-3.9.md): what the analysers of core/include/dect2/insight report, and small helpers they share.
// Everything here works on what this receiver decoded; nothing goes to the internet.
#pragma once
#include <cmath>
#include <cctype>
#include <string>

namespace dect2::insight {

// One finding: an alert or a change worth showing
struct Insight {
    double time = 0;            // Unix seconds (the event's time)
    std::string area;           // "ham", "voice", "pager", "aviation", "marine", "iot"
    std::string kind;           // "heard_me", "emergency_descent", "ducting" ...
    int severity = 0;           // 0 information, 1 warning, 2 alarm
    std::string key;            // what it is about (a call sign, a talkgroup, an ICAO address, a network), for de-duplication
    std::string text;           // one line for the list
    bool hasPos = false;
    double lat = 0, lon = 0;
};

constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthKm = 6371.0;

inline double rad(double d) { return d * kPi / 180.0; }
inline double deg(double r) { return r * 180.0 / kPi; }

// great-circle distance (km)
inline double distanceKm(double lat1, double lon1, double lat2, double lon2) {
    const double dl = rad(lat2 - lat1), dn = rad(lon2 - lon1);
    const double a = std::sin(dl / 2) * std::sin(dl / 2) + std::cos(rad(lat1)) * std::cos(rad(lat2)) * std::sin(dn / 2) * std::sin(dn / 2);
    return 2 * kEarthKm * std::asin(std::min(1.0, std::sqrt(a)));
}

// initial bearing from point 1 to point 2, degrees 0 .. 360
inline double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
    const double y = std::sin(rad(lon2 - lon1)) * std::cos(rad(lat2));
    const double x = std::cos(rad(lat1)) * std::sin(rad(lat2)) - std::sin(rad(lat1)) * std::cos(rad(lat2)) * std::cos(rad(lon2 - lon1));
    double b = deg(std::atan2(y, x));
    if (b < 0) b += 360;
    return b;
}

// the point `km` away along bearing `brgDeg`
inline void destination(double lat, double lon, double brgDeg, double km, double& outLat, double& outLon) {
    const double d = km / kEarthKm, b = rad(brgDeg), p1 = rad(lat), l1 = rad(lon);
    const double p2 = std::asin(std::sin(p1) * std::cos(d) + std::cos(p1) * std::sin(d) * std::cos(b));
    const double l2 = l1 + std::atan2(std::sin(b) * std::sin(d) * std::cos(p1), std::cos(d) - std::sin(p1) * std::sin(p2));
    outLat = deg(p2);
    outLon = std::remainder(deg(l2), 360.0);
}

// the middle of a 4- or 6-character Maidenhead locator; false when it is not one
inline bool gridToLatLon(const std::string& g, double& lat, double& lon) {
    if (g.size() != 4 && g.size() != 6) return false;
    const int A = std::toupper((unsigned char)g[0]) - 'A', B = std::toupper((unsigned char)g[1]) - 'A';
    if (A < 0 || A > 17 || B < 0 || B > 17 || !std::isdigit((unsigned char)g[2]) || !std::isdigit((unsigned char)g[3])) return false;
    lon = -180 + A * 20 + (g[2] - '0') * 2;
    lat = -90 + B * 10 + (g[3] - '0');
    if (g.size() == 6) {
        const int c = std::tolower((unsigned char)g[4]) - 'a', d = std::tolower((unsigned char)g[5]) - 'a';
        if (c < 0 || c > 23 || d < 0 || d > 23) return false;
        lon += c * (2.0 / 24) + 1.0 / 24;
        lat += d * (1.0 / 24) + 0.5 / 24;
    } else {
        lon += 1;
        lat += 0.5;
    }
    if (g == "RR73" || g == "rr73") return false;   // FT8's "RR73" looks like a locator in the Arctic Ocean; it is not one
    return true;
}

// "NE" for 45 degrees: 8 compass points
inline const char* compass8(double brg) {
    static const char* const n[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    int i = (int)std::floor(std::fmod(brg + 22.5 + 360.0, 360.0) / 45.0);
    return n[i & 7];
}

inline std::string upper(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }

}  // namespace dect2::insight
