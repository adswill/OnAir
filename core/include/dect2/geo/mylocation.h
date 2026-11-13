// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace dect2 { namespace myloc {

constexpr double kEarthKm = 6371.0088;
constexpr double kNmKm = 1.852;
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

inline bool validPosition(double lat, double lon) {
    return std::isfinite(lat) && std::isfinite(lon) && lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 360.0;
}

inline double wrap180(double lon) {
    lon = std::fmod(lon + 180.0, 360.0);
    if (lon < 0) lon += 360.0;
    return lon - 180.0;
}

inline double distanceKm(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = lat1 * kDeg, p2 = lat2 * kDeg, dp = p2 - p1, dl = (lon2 - lon1) * kDeg;
    const double s1 = std::sin(dp / 2), s2 = std::sin(dl / 2);
    const double h = std::min(1.0, std::max(0.0, s1 * s1 + std::cos(p1) * std::cos(p2) * s2 * s2));
    return 2.0 * kEarthKm * std::asin(std::sqrt(h));
}

inline double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = lat1 * kDeg, p2 = lat2 * kDeg, dl = (lon2 - lon1) * kDeg;
    const double y = std::sin(dl) * std::cos(p2);
    const double x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
    if (std::fabs(x) < 1e-15 && std::fabs(y) < 1e-15) return lat1 > lat2 ? 180.0 : 0.0;
    double b = std::atan2(y, x) / kDeg;
    b = std::fmod(b + 360.0, 360.0);
    return b >= 360.0 ? 0.0 : b;
}

inline void destination(double lat, double lon, double bearing, double km, double& outLat, double& outLon) {
    const double d = km / kEarthKm, b = bearing * kDeg, p1 = lat * kDeg;
    const double sp = std::min(1.0, std::max(-1.0, std::sin(p1) * std::cos(d) + std::cos(p1) * std::sin(d) * std::cos(b)));
    const double p2 = std::asin(sp);
    const double l2 = lon * kDeg + std::atan2(std::sin(b) * std::sin(d) * std::cos(p1), std::cos(d) - std::sin(p1) * sp);
    outLat = p2 / kDeg;
    outLon = wrap180(l2 / kDeg);
}

inline std::vector<std::pair<double, double>> ringPoints(double lat, double lon, double km, int segments) {
    std::vector<std::pair<double, double>> out;
    if (!(km > 0) || segments < 3 || !validPosition(lat, lon)) return out;
    const double maxKm = kPi * kEarthKm;
    if (km > maxKm) km = maxKm;
    out.reserve((size_t)segments + 1);
    for (int i = 0; i <= segments; i++) {
        double la, lo;
        destination(lat, lon, 360.0 * i / segments, km, la, lo);
        out.emplace_back(la, lo);
    }
    return out;
}

inline int ringSegments(double radiusPx) {
    const int n = (int)(radiusPx / 4.0);
    return std::max(24, std::min(180, n));
}

inline const char* compass(double bearing) {
    static const char* const k[16] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
    double b = std::fmod(bearing, 360.0);
    if (b < 0) b += 360.0;
    return k[((int)std::floor(b / 22.5 + 0.5)) % 16];
}

inline std::vector<double> parseRingList(const std::string& text) {
    std::vector<double> v;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty()) {
            char* end = nullptr;
            const double x = std::strtod(cur.c_str(), &end);
            if (end != cur.c_str() && std::isfinite(x) && x > 0 && x < 40000) v.push_back(x);
        }
        cur.clear();
    };
    for (char c : text) {
        if (c == ',' || c == ';' || c == ' ' || c == '\t') flush();
        else cur += c;
    }
    flush();
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    if (v.size() > 8) v.resize(8);
    return v;
}

inline double toKm(double value, bool metric) { return metric ? value : value * kNmKm; }

inline std::string formatDistance(double km, bool metric) {
    const double v = metric ? km : km / kNmKm;
    char b[48];
    if (v < 10) std::snprintf(b, sizeof b, "%.1f %s", v, metric ? "km" : "nm");
    else std::snprintf(b, sizeof b, "%.0f %s", v, metric ? "km" : "nm");
    return b;
}

inline std::string formatBearing(double bearing) {
    char b[32];
    std::snprintf(b, sizeof b, "%03.0f\xC2\xB0 %s", bearing >= 359.5 ? 0.0 : bearing, compass(bearing));
    return b;
}

inline std::string describe(double myLat, double myLon, double lat, double lon, bool metric) {
    if (!validPosition(myLat, myLon) || !validPosition(lat, lon)) return "";
    return formatDistance(distanceKm(myLat, myLon, lat, lon), metric) + " at " + formatBearing(bearingDeg(myLat, myLon, lat, lon));
}

} }
